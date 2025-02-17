//===-- LibCxxMap.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "LibCxx.h"

#include "Plugins/TypeSystem/Clang/TypeSystemClang.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Core/ValueObjectConstResult.h"
#include "lldb/DataFormatters/FormattersHelpers.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/Endian.h"
#include "lldb/Utility/Status.h"
#include "lldb/Utility/Stream.h"
#include "lldb/lldb-forward.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;

class MapEntry {
public:
  MapEntry() = default;
  explicit MapEntry(ValueObjectSP entry_sp) : m_entry_sp(entry_sp) {}
  explicit MapEntry(ValueObject *entry)
      : m_entry_sp(entry ? entry->GetSP() : ValueObjectSP()) {}

  ValueObjectSP left() const {
    if (!m_entry_sp)
      return m_entry_sp;
    return m_entry_sp->GetSyntheticChildAtOffset(
        0, m_entry_sp->GetCompilerType(), true);
  }

  ValueObjectSP right() const {
    if (!m_entry_sp)
      return m_entry_sp;
    return m_entry_sp->GetSyntheticChildAtOffset(
        m_entry_sp->GetProcessSP()->GetAddressByteSize(),
        m_entry_sp->GetCompilerType(), true);
  }

  ValueObjectSP parent() const {
    if (!m_entry_sp)
      return m_entry_sp;
    return m_entry_sp->GetSyntheticChildAtOffset(
        2 * m_entry_sp->GetProcessSP()->GetAddressByteSize(),
        m_entry_sp->GetCompilerType(), true);
  }

  uint64_t value() const {
    if (!m_entry_sp)
      return 0;
    return m_entry_sp->GetValueAsUnsigned(0);
  }

  bool error() const {
    if (!m_entry_sp)
      return true;
    return m_entry_sp->GetError().Fail();
  }

  bool null() const { return (value() == 0); }

  ValueObjectSP GetEntry() const { return m_entry_sp; }

  void SetEntry(ValueObjectSP entry) { m_entry_sp = entry; }

  bool operator==(const MapEntry &rhs) const {
    return (rhs.m_entry_sp.get() == m_entry_sp.get());
  }

private:
  ValueObjectSP m_entry_sp;
};

class MapIterator {
public:
  MapIterator(ValueObject *entry, size_t depth = 0)
      : m_entry(entry), m_max_depth(depth), m_error(false) {}

  MapIterator() = default;

  ValueObjectSP value() { return m_entry.GetEntry(); }

  ValueObjectSP advance(size_t count) {
    ValueObjectSP fail;
    if (m_error)
      return fail;
    size_t steps = 0;
    while (count > 0) {
      next();
      count--, steps++;
      if (m_error || m_entry.null() || (steps > m_max_depth))
        return fail;
    }
    return m_entry.GetEntry();
  }

private:
  /// Mimicks libc++'s __tree_next algorithm, which libc++ uses
  /// in its __tree_iteartor::operator++.
  void next() {
    if (m_entry.null())
      return;
    MapEntry right(m_entry.right());
    if (!right.null()) {
      m_entry = tree_min(std::move(right));
      return;
    }
    size_t steps = 0;
    while (!is_left_child(m_entry)) {
      if (m_entry.error()) {
        m_error = true;
        return;
      }
      m_entry.SetEntry(m_entry.parent());
      steps++;
      if (steps > m_max_depth) {
        m_entry = MapEntry();
        return;
      }
    }
    m_entry = MapEntry(m_entry.parent());
  }

  /// Mimicks libc++'s __tree_min algorithm.
  MapEntry tree_min(MapEntry x) {
    if (x.null())
      return MapEntry();
    MapEntry left(x.left());
    size_t steps = 0;
    while (!left.null()) {
      if (left.error()) {
        m_error = true;
        return MapEntry();
      }
      x = left;
      left.SetEntry(x.left());
      steps++;
      if (steps > m_max_depth)
        return MapEntry();
    }
    return x;
  }

  bool is_left_child(const MapEntry &x) {
    if (x.null())
      return false;
    MapEntry rhs(x.parent());
    rhs.SetEntry(rhs.left());
    return x.value() == rhs.value();
  }

  MapEntry m_entry;
  size_t m_max_depth = 0;
  bool m_error = false;
};

namespace lldb_private {
namespace formatters {
class LibcxxStdMapSyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  LibcxxStdMapSyntheticFrontEnd(lldb::ValueObjectSP valobj_sp);

  ~LibcxxStdMapSyntheticFrontEnd() override = default;

  llvm::Expected<uint32_t> CalculateNumChildren() override;

  lldb::ValueObjectSP GetChildAtIndex(uint32_t idx) override;

  lldb::ChildCacheState Update() override;

  bool MightHaveChildren() override;

  size_t GetIndexOfChildWithName(ConstString name) override;

private:
  bool GetDataType();

  void GetValueOffset(const lldb::ValueObjectSP &node);

  /// Returns the ValueObject for the __tree_node type that
  /// holds the key/value pair of the node at index \ref idx.
  ///
  /// \param[in] idx The child index that we're looking to get
  ///                the key/value pair for.
  ///
  /// \param[in] max_depth The maximum search depth after which
  ///                      we stop trying to find the key/value
  ///                      pair for.
  ///
  /// \returns On success, returns the ValueObjectSP corresponding
  ///          to the __tree_node's __value_ member (which holds
  ///          the key/value pair the formatter wants to display).
  ///          On failure, will return nullptr.
  ValueObjectSP GetKeyValuePair(size_t idx, size_t max_depth);

  ValueObject *m_tree = nullptr;
  ValueObject *m_root_node = nullptr;
  CompilerType m_element_type;
  uint32_t m_skip_size = UINT32_MAX;
  size_t m_count = UINT32_MAX;
  std::map<size_t, MapIterator> m_iterators;
};

class LibCxxMapIteratorSyntheticFrontEnd : public SyntheticChildrenFrontEnd {
public:
  LibCxxMapIteratorSyntheticFrontEnd(lldb::ValueObjectSP valobj_sp);

  llvm::Expected<uint32_t> CalculateNumChildren() override;

  lldb::ValueObjectSP GetChildAtIndex(uint32_t idx) override;

  lldb::ChildCacheState Update() override;

  bool MightHaveChildren() override;

  size_t GetIndexOfChildWithName(ConstString name) override;

  ~LibCxxMapIteratorSyntheticFrontEnd() override;

private:
  ValueObject *m_pair_ptr;
  lldb::ValueObjectSP m_pair_sp;
};
} // namespace formatters
} // namespace lldb_private

lldb_private::formatters::LibcxxStdMapSyntheticFrontEnd::
    LibcxxStdMapSyntheticFrontEnd(lldb::ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp), m_element_type(), m_iterators() {
  if (valobj_sp)
    Update();
}

llvm::Expected<uint32_t> lldb_private::formatters::
    LibcxxStdMapSyntheticFrontEnd::CalculateNumChildren() {
  if (m_count != UINT32_MAX)
    return m_count;

  if (m_tree == nullptr)
    return 0;

  ValueObjectSP size_node(m_tree->GetChildMemberWithName("__pair3_"));
  if (!size_node)
    return 0;

  size_node = GetFirstValueOfLibCXXCompressedPair(*size_node);

  if (!size_node)
    return 0;

  m_count = size_node->GetValueAsUnsigned(0);
  return m_count;
}

bool lldb_private::formatters::LibcxxStdMapSyntheticFrontEnd::GetDataType() {
  if (m_element_type.IsValid())
    return true;
  m_element_type.Clear();
  ValueObjectSP deref;
  Status error;
  deref = m_root_node->Dereference(error);
  if (!deref || error.Fail())
    return false;
  deref = m_backend.GetChildAtNamePath({"__tree_", "__pair3_"});
  if (!deref)
    return false;
  m_element_type = deref->GetCompilerType()
                       .GetTypeTemplateArgument(1)
                       .GetTypeTemplateArgument(1);
  if (m_element_type) {
    std::string name;
    uint64_t bit_offset_ptr;
    uint32_t bitfield_bit_size_ptr;
    bool is_bitfield_ptr;
    m_element_type = m_element_type.GetFieldAtIndex(
        0, name, &bit_offset_ptr, &bitfield_bit_size_ptr, &is_bitfield_ptr);
    m_element_type = m_element_type.GetTypedefedType();
    return m_element_type.IsValid();
  } else {
    m_element_type = m_backend.GetCompilerType().GetTypeTemplateArgument(0);
    return m_element_type.IsValid();
  }
}

void lldb_private::formatters::LibcxxStdMapSyntheticFrontEnd::GetValueOffset(
    const lldb::ValueObjectSP &node) {
  if (m_skip_size != UINT32_MAX)
    return;
  if (!node)
    return;

  CompilerType node_type(node->GetCompilerType());
  auto ast_ctx = node_type.GetTypeSystem().dyn_cast_or_null<TypeSystemClang>();
  if (!ast_ctx)
    return;

  CompilerType tree_node_type = ast_ctx->CreateStructForIdentifier(
      llvm::StringRef(),
      {{"ptr0", ast_ctx->GetBasicType(lldb::eBasicTypeVoid).GetPointerType()},
       {"ptr1", ast_ctx->GetBasicType(lldb::eBasicTypeVoid).GetPointerType()},
       {"ptr2", ast_ctx->GetBasicType(lldb::eBasicTypeVoid).GetPointerType()},
       {"cw", ast_ctx->GetBasicType(lldb::eBasicTypeBool)},
       {"payload", (m_element_type.GetCompleteType(), m_element_type)}});
  std::string child_name;
  uint32_t child_byte_size;
  int32_t child_byte_offset = 0;
  uint32_t child_bitfield_bit_size;
  uint32_t child_bitfield_bit_offset;
  bool child_is_base_class;
  bool child_is_deref_of_parent;
  uint64_t language_flags;
  auto child_type =
      llvm::expectedToStdOptional(tree_node_type.GetChildCompilerTypeAtIndex(
          nullptr, 4, true, true, true, child_name, child_byte_size,
          child_byte_offset, child_bitfield_bit_size, child_bitfield_bit_offset,
          child_is_base_class, child_is_deref_of_parent, nullptr,
          language_flags));
  if (child_type && child_type->IsValid())
    m_skip_size = (uint32_t)child_byte_offset;
}

ValueObjectSP
lldb_private::formatters::LibcxxStdMapSyntheticFrontEnd::GetKeyValuePair(
    size_t idx, size_t max_depth) {
  MapIterator iterator(m_root_node, max_depth);

  const bool need_to_skip = (idx > 0);
  size_t actual_advance = idx;
  if (need_to_skip) {
    // If we have already created the iterator for the previous
    // index, we can start from there and advance by 1.
    auto cached_iterator = m_iterators.find(idx - 1);
    if (cached_iterator != m_iterators.end()) {
      iterator = cached_iterator->second;
      actual_advance = 1;
    }
  }

  ValueObjectSP iterated_sp(iterator.advance(actual_advance));
  if (!iterated_sp)
    // this tree is garbage - stop
    return nullptr;

  if (!GetDataType())
    return nullptr;

  if (!need_to_skip) {
    Status error;
    iterated_sp = iterated_sp->Dereference(error);
    if (!iterated_sp || error.Fail())
      return nullptr;

    GetValueOffset(iterated_sp);
    iterated_sp = iterated_sp->GetSyntheticChildAtOffset(m_skip_size,
                                                         m_element_type, true);

    if (!iterated_sp)
      return nullptr;
  } else {
    // because of the way our debug info is made, we need to read item 0
    // first so that we can cache information used to generate other elements
    if (m_skip_size == UINT32_MAX)
      GetChildAtIndex(0);

    if (m_skip_size == UINT32_MAX)
      return nullptr;

    iterated_sp = iterated_sp->GetSyntheticChildAtOffset(m_skip_size,
                                                         m_element_type, true);
    if (!iterated_sp)
      return nullptr;
  }

  m_iterators[idx] = iterator;
  assert(iterated_sp != nullptr &&
         "Cached MapIterator for invalid ValueObject");

  return iterated_sp;
}

lldb::ValueObjectSP
lldb_private::formatters::LibcxxStdMapSyntheticFrontEnd::GetChildAtIndex(
    uint32_t idx) {
  static ConstString g_cc_("__cc_"), g_cc("__cc");
  static ConstString g_nc("__nc");
  uint32_t num_children = CalculateNumChildrenIgnoringErrors();
  if (idx >= num_children)
    return nullptr;

  if (m_tree == nullptr || m_root_node == nullptr)
    return nullptr;

  ValueObjectSP key_val_sp = GetKeyValuePair(idx, /*max_depth=*/num_children);
  if (!key_val_sp) {
    // this will stop all future searches until an Update() happens
    m_tree = nullptr;
    return nullptr;
  }

  // at this point we have a valid
  // we need to copy current_sp into a new object otherwise we will end up with
  // all items named __value_
  StreamString name;
  name.Printf("[%" PRIu64 "]", (uint64_t)idx);
  auto potential_child_sp = key_val_sp->Clone(ConstString(name.GetString()));
  if (potential_child_sp) {
    switch (potential_child_sp->GetNumChildrenIgnoringErrors()) {
    case 1: {
      auto child0_sp = potential_child_sp->GetChildAtIndex(0);
      if (child0_sp &&
          (child0_sp->GetName() == g_cc_ || child0_sp->GetName() == g_cc))
        potential_child_sp = child0_sp->Clone(ConstString(name.GetString()));
      break;
    }
    case 2: {
      auto child0_sp = potential_child_sp->GetChildAtIndex(0);
      auto child1_sp = potential_child_sp->GetChildAtIndex(1);
      if (child0_sp &&
          (child0_sp->GetName() == g_cc_ || child0_sp->GetName() == g_cc) &&
          child1_sp && child1_sp->GetName() == g_nc)
        potential_child_sp = child0_sp->Clone(ConstString(name.GetString()));
      break;
    }
    }
  }
  return potential_child_sp;
}

lldb::ChildCacheState
lldb_private::formatters::LibcxxStdMapSyntheticFrontEnd::Update() {
  m_count = UINT32_MAX;
  m_tree = m_root_node = nullptr;
  m_iterators.clear();
  m_tree = m_backend.GetChildMemberWithName("__tree_").get();
  if (!m_tree)
    return lldb::ChildCacheState::eRefetch;
  m_root_node = m_tree->GetChildMemberWithName("__begin_node_").get();
  return lldb::ChildCacheState::eRefetch;
}

bool lldb_private::formatters::LibcxxStdMapSyntheticFrontEnd::
    MightHaveChildren() {
  return true;
}

size_t lldb_private::formatters::LibcxxStdMapSyntheticFrontEnd::
    GetIndexOfChildWithName(ConstString name) {
  return ExtractIndexFromString(name.GetCString());
}

SyntheticChildrenFrontEnd *
lldb_private::formatters::LibcxxStdMapSyntheticFrontEndCreator(
    CXXSyntheticChildren *, lldb::ValueObjectSP valobj_sp) {
  return (valobj_sp ? new LibcxxStdMapSyntheticFrontEnd(valobj_sp) : nullptr);
}

lldb_private::formatters::LibCxxMapIteratorSyntheticFrontEnd::
    LibCxxMapIteratorSyntheticFrontEnd(lldb::ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp), m_pair_ptr(), m_pair_sp() {
  if (valobj_sp)
    Update();
}

lldb::ChildCacheState
lldb_private::formatters::LibCxxMapIteratorSyntheticFrontEnd::Update() {
  m_pair_sp.reset();
  m_pair_ptr = nullptr;

  ValueObjectSP valobj_sp = m_backend.GetSP();
  if (!valobj_sp)
    return lldb::ChildCacheState::eRefetch;

  TargetSP target_sp(valobj_sp->GetTargetSP());

  if (!target_sp)
    return lldb::ChildCacheState::eRefetch;

  // this must be a ValueObject* because it is a child of the ValueObject we
  // are producing children for it if were a ValueObjectSP, we would end up
  // with a loop (iterator -> synthetic -> child -> parent == iterator) and
  // that would in turn leak memory by never allowing the ValueObjects to die
  // and free their memory
  m_pair_ptr = valobj_sp
                   ->GetValueForExpressionPath(
                       ".__i_.__ptr_->__value_", nullptr, nullptr,
                       ValueObject::GetValueForExpressionPathOptions()
                           .DontCheckDotVsArrowSyntax()
                           .SetSyntheticChildrenTraversal(
                               ValueObject::GetValueForExpressionPathOptions::
                                   SyntheticChildrenTraversal::None),
                       nullptr)
                   .get();

  if (!m_pair_ptr) {
    m_pair_ptr = valobj_sp
                     ->GetValueForExpressionPath(
                         ".__i_.__ptr_", nullptr, nullptr,
                         ValueObject::GetValueForExpressionPathOptions()
                             .DontCheckDotVsArrowSyntax()
                             .SetSyntheticChildrenTraversal(
                                 ValueObject::GetValueForExpressionPathOptions::
                                     SyntheticChildrenTraversal::None),
                         nullptr)
                     .get();
    if (m_pair_ptr) {
      auto __i_(valobj_sp->GetChildMemberWithName("__i_"));
      if (!__i_) {
        m_pair_ptr = nullptr;
        return lldb::ChildCacheState::eRefetch;
      }
      CompilerType pair_type(
          __i_->GetCompilerType().GetTypeTemplateArgument(0));
      std::string name;
      uint64_t bit_offset_ptr;
      uint32_t bitfield_bit_size_ptr;
      bool is_bitfield_ptr;
      pair_type = pair_type.GetFieldAtIndex(
          0, name, &bit_offset_ptr, &bitfield_bit_size_ptr, &is_bitfield_ptr);
      if (!pair_type) {
        m_pair_ptr = nullptr;
        return lldb::ChildCacheState::eRefetch;
      }

      auto addr(m_pair_ptr->GetValueAsUnsigned(LLDB_INVALID_ADDRESS));
      m_pair_ptr = nullptr;
      if (addr && addr != LLDB_INVALID_ADDRESS) {
        auto ts = pair_type.GetTypeSystem();
        auto ast_ctx = ts.dyn_cast_or_null<TypeSystemClang>();
        if (!ast_ctx)
          return lldb::ChildCacheState::eRefetch;

        // Mimick layout of std::__tree_iterator::__ptr_ and read it in
        // from process memory.
        //
        // The following shows the contiguous block of memory:
        //
        //        +-----------------------------+ class __tree_end_node
        // __ptr_ | pointer __left_;            |
        //        +-----------------------------+ class __tree_node_base
        //        | pointer __right_;           |
        //        | __parent_pointer __parent_; |
        //        | bool __is_black_;           |
        //        +-----------------------------+ class __tree_node
        //        | __node_value_type __value_; | <<< our key/value pair
        //        +-----------------------------+
        //
        CompilerType tree_node_type = ast_ctx->CreateStructForIdentifier(
            llvm::StringRef(),
            {{"ptr0",
              ast_ctx->GetBasicType(lldb::eBasicTypeVoid).GetPointerType()},
             {"ptr1",
              ast_ctx->GetBasicType(lldb::eBasicTypeVoid).GetPointerType()},
             {"ptr2",
              ast_ctx->GetBasicType(lldb::eBasicTypeVoid).GetPointerType()},
             {"cw", ast_ctx->GetBasicType(lldb::eBasicTypeBool)},
             {"payload", pair_type}});
        std::optional<uint64_t> size = tree_node_type.GetByteSize(nullptr);
        if (!size)
          return lldb::ChildCacheState::eRefetch;
        WritableDataBufferSP buffer_sp(new DataBufferHeap(*size, 0));
        ProcessSP process_sp(target_sp->GetProcessSP());
        Status error;
        process_sp->ReadMemory(addr, buffer_sp->GetBytes(),
                               buffer_sp->GetByteSize(), error);
        if (error.Fail())
          return lldb::ChildCacheState::eRefetch;
        DataExtractor extractor(buffer_sp, process_sp->GetByteOrder(),
                                process_sp->GetAddressByteSize());
        auto pair_sp = CreateValueObjectFromData(
            "pair", extractor, valobj_sp->GetExecutionContextRef(),
            tree_node_type);
        if (pair_sp)
          m_pair_sp = pair_sp->GetChildAtIndex(4);
      }
    }
  }

  return lldb::ChildCacheState::eRefetch;
}

llvm::Expected<uint32_t> lldb_private::formatters::
    LibCxxMapIteratorSyntheticFrontEnd::CalculateNumChildren() {
  return 2;
}

lldb::ValueObjectSP
lldb_private::formatters::LibCxxMapIteratorSyntheticFrontEnd::GetChildAtIndex(
    uint32_t idx) {
  if (m_pair_ptr)
    return m_pair_ptr->GetChildAtIndex(idx);
  if (m_pair_sp)
    return m_pair_sp->GetChildAtIndex(idx);
  return lldb::ValueObjectSP();
}

bool lldb_private::formatters::LibCxxMapIteratorSyntheticFrontEnd::
    MightHaveChildren() {
  return true;
}

size_t lldb_private::formatters::LibCxxMapIteratorSyntheticFrontEnd::
    GetIndexOfChildWithName(ConstString name) {
  if (name == "first")
    return 0;
  if (name == "second")
    return 1;
  return UINT32_MAX;
}

lldb_private::formatters::LibCxxMapIteratorSyntheticFrontEnd::
    ~LibCxxMapIteratorSyntheticFrontEnd() {
  // this will be deleted when its parent dies (since it's a child object)
  // delete m_pair_ptr;
}

SyntheticChildrenFrontEnd *
lldb_private::formatters::LibCxxMapIteratorSyntheticFrontEndCreator(
    CXXSyntheticChildren *, lldb::ValueObjectSP valobj_sp) {
  return (valobj_sp ? new LibCxxMapIteratorSyntheticFrontEnd(valobj_sp)
                    : nullptr);
}
