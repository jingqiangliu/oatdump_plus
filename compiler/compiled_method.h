/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_COMPILED_METHOD_H_
#define ART_COMPILER_COMPILED_METHOD_H_

#include <memory>
#include <string>
#include <vector>

#include "arch/instruction_set.h"
#include "method_reference.h"
#include "utils.h"
#include "utils/array_ref.h"
#include "utils/swap_space.h"

namespace art {

class CompilerDriver;

class CompiledCode {
 public:
  // For Quick to supply an code blob
  CompiledCode(CompilerDriver* compiler_driver, InstructionSet instruction_set,
               const ArrayRef<const uint8_t>& quick_code, bool owns_code_array);

  virtual ~CompiledCode();

  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }

  const SwapVector<uint8_t>* GetQuickCode() const {
    return quick_code_;
  }

  void SetCode(const ArrayRef<const uint8_t>* quick_code);

  bool operator==(const CompiledCode& rhs) const;

  // To align an offset from a page-aligned value to make it suitable
  // for code storage. For example on ARM, to ensure that PC relative
  // valu computations work out as expected.
  size_t AlignCode(size_t offset) const;
  static size_t AlignCode(size_t offset, InstructionSet instruction_set);

  // returns the difference between the code address and a usable PC.
  // mainly to cope with kThumb2 where the lower bit must be set.
  size_t CodeDelta() const;
  static size_t CodeDelta(InstructionSet instruction_set);

  // Returns a pointer suitable for invoking the code at the argument
  // code_pointer address.  Mainly to cope with kThumb2 where the
  // lower bit must be set to indicate Thumb mode.
  static const void* CodePointer(const void* code_pointer,
                                 InstructionSet instruction_set);

  const std::vector<uint32_t>& GetOatdataOffsetsToCompliledCodeOffset() const;
  void AddOatdataOffsetToCompliledCodeOffset(uint32_t offset);

 private:
  CompilerDriver* const compiler_driver_;

  const InstructionSet instruction_set_;

  // If we own the code array (means that we free in destructor).
  const bool owns_code_array_;

  // Used to store the PIC code for Quick.
  SwapVector<uint8_t>* quick_code_;

  // There are offsets from the oatdata symbol to where the offset to
  // the compiled method will be found. These are computed by the
  // OatWriter and then used by the ElfWriter to add relocations so
  // that MCLinker can update the values to the location in the linked .so.
  std::vector<uint32_t> oatdata_offsets_to_compiled_code_offset_;
};

class SrcMapElem {
 public:
  uint32_t from_;
  int32_t to_;

  explicit operator int64_t() const {
    return (static_cast<int64_t>(to_) << 32) | from_;
  }

  bool operator<(const SrcMapElem& sme) const {
    return int64_t(*this) < int64_t(sme);
  }

  bool operator==(const SrcMapElem& sme) const {
    return int64_t(*this) == int64_t(sme);
  }

  explicit operator uint8_t() const {
    return static_cast<uint8_t>(from_ + to_);
  }
};

template <class Allocator>
class SrcMap FINAL : public std::vector<SrcMapElem, Allocator> {
 public:
  using std::vector<SrcMapElem, Allocator>::begin;
  using typename std::vector<SrcMapElem, Allocator>::const_iterator;
  using std::vector<SrcMapElem, Allocator>::empty;
  using std::vector<SrcMapElem, Allocator>::end;
  using std::vector<SrcMapElem, Allocator>::resize;
  using std::vector<SrcMapElem, Allocator>::shrink_to_fit;
  using std::vector<SrcMapElem, Allocator>::size;

  explicit SrcMap() {}
  explicit SrcMap(const Allocator& alloc) : std::vector<SrcMapElem, Allocator>(alloc) {}

  template <class InputIt>
  SrcMap(InputIt first, InputIt last, const Allocator& alloc)
      : std::vector<SrcMapElem, Allocator>(first, last, alloc) {}

  void SortByFrom() {
    std::sort(begin(), end(), [] (const SrcMapElem& lhs, const SrcMapElem& rhs) -> bool {
      return lhs.from_ < rhs.from_;
    });
  }

  const_iterator FindByTo(int32_t to) const {
    return std::lower_bound(begin(), end(), SrcMapElem({0, to}));
  }

  SrcMap& Arrange() {
    if (!empty()) {
      std::sort(begin(), end());
      resize(std::unique(begin(), end()) - begin());
      shrink_to_fit();
    }
    return *this;
  }

  void DeltaFormat(const SrcMapElem& start, uint32_t highest_pc) {
    // Convert from abs values to deltas.
    if (!empty()) {
      SortByFrom();

      // TODO: one PC can be mapped to several Java src lines.
      // do we want such a one-to-many correspondence?

      // get rid of the highest values
      size_t i = size() - 1;
      for (; i > 0 ; i--) {
        if ((*this)[i].from_ < highest_pc) {
          break;
        }
      }
      this->resize(i + 1);

      for (i = size(); --i >= 1; ) {
        (*this)[i].from_ -= (*this)[i-1].from_;
        (*this)[i].to_ -= (*this)[i-1].to_;
      }
      DCHECK((*this)[0].from_ >= start.from_);
      (*this)[0].from_ -= start.from_;
      (*this)[0].to_ -= start.to_;
    }
  }
};

using DefaultSrcMap = SrcMap<std::allocator<SrcMapElem>>;
using SwapSrcMap = SrcMap<SwapAllocator<SrcMapElem>>;


enum LinkerPatchType {
  kLinkerPatchMethod,
  kLinkerPatchCall,
  kLinkerPatchCallRelative,  // NOTE: Actual patching is instruction_set-dependent.
  kLinkerPatchType,
};

class LinkerPatch {
 public:
  static LinkerPatch MethodPatch(size_t literal_offset,
                                 const DexFile* target_dex_file,
                                 uint32_t target_method_idx) {
    return LinkerPatch(literal_offset, kLinkerPatchMethod,
                       target_method_idx, target_dex_file);
  }

  static LinkerPatch CodePatch(size_t literal_offset,
                               const DexFile* target_dex_file,
                               uint32_t target_method_idx) {
    return LinkerPatch(literal_offset, kLinkerPatchCall,
                       target_method_idx, target_dex_file);
  }

  static LinkerPatch RelativeCodePatch(size_t literal_offset,
                                       const DexFile* target_dex_file,
                                       uint32_t target_method_idx) {
    return LinkerPatch(literal_offset, kLinkerPatchCallRelative,
                       target_method_idx, target_dex_file);
  }

  static LinkerPatch TypePatch(size_t literal_offset,
                               const DexFile* target_dex_file,
                               uint32_t target_type_idx) {
    return LinkerPatch(literal_offset, kLinkerPatchType, target_type_idx, target_dex_file);
  }

  LinkerPatch(const LinkerPatch& other) = default;
  LinkerPatch& operator=(const LinkerPatch& other) = default;

  size_t LiteralOffset() const {
    return literal_offset_;
  }

  LinkerPatchType Type() const {
    return patch_type_;
  }

  MethodReference TargetMethod() const {
    DCHECK(patch_type_ == kLinkerPatchMethod ||
           patch_type_ == kLinkerPatchCall || patch_type_ == kLinkerPatchCallRelative);
    return MethodReference(target_dex_file_, target_idx_);
  }

  const DexFile* TargetTypeDexFile() const {
    DCHECK(patch_type_ == kLinkerPatchType);
    return target_dex_file_;
  }

  uint32_t TargetTypeIndex() const {
    DCHECK(patch_type_ == kLinkerPatchType);
    return target_idx_;
  }

 private:
  LinkerPatch(size_t literal_offset, LinkerPatchType patch_type,
              uint32_t target_idx, const DexFile* target_dex_file)
      : literal_offset_(literal_offset),
        patch_type_(patch_type),
        target_idx_(target_idx),
        target_dex_file_(target_dex_file) {
  }

  size_t literal_offset_;
  LinkerPatchType patch_type_;
  uint32_t target_idx_;  // Method index (Call/Method patches) or type index (Type patches).
  const DexFile* target_dex_file_;

  friend bool operator==(const LinkerPatch& lhs, const LinkerPatch& rhs);
  friend bool operator<(const LinkerPatch& lhs, const LinkerPatch& rhs);
};

inline bool operator==(const LinkerPatch& lhs, const LinkerPatch& rhs) {
  return lhs.literal_offset_ == rhs.literal_offset_ &&
      lhs.patch_type_ == rhs.patch_type_ &&
      lhs.target_idx_ == rhs.target_idx_ &&
      lhs.target_dex_file_ == rhs.target_dex_file_;
}

inline bool operator<(const LinkerPatch& lhs, const LinkerPatch& rhs) {
  return (lhs.literal_offset_ != rhs.literal_offset_) ? lhs.literal_offset_ < rhs.literal_offset_
      : (lhs.patch_type_ != rhs.patch_type_) ? lhs.patch_type_ < rhs.patch_type_
      : (lhs.target_idx_ != rhs.target_idx_) ? lhs.target_idx_ < rhs.target_idx_
      : lhs.target_dex_file_ < rhs.target_dex_file_;
}

class CompiledMethod FINAL : public CompiledCode {
 public:
  // Constructs a CompiledMethod.
  // Note: Consider using the static allocation methods below that will allocate the CompiledMethod
  //       in the swap space.
  CompiledMethod(CompilerDriver* driver,
                 InstructionSet instruction_set,
                 const ArrayRef<const uint8_t>& quick_code,
                 const size_t frame_size_in_bytes,
                 const uint32_t core_spill_mask,
                 const uint32_t fp_spill_mask,
                 DefaultSrcMap* src_mapping_table,
                 const ArrayRef<const uint8_t>& mapping_table,
                 const ArrayRef<const uint8_t>& vmap_table,
                 const ArrayRef<const uint8_t>& native_gc_map,
                 const ArrayRef<const uint8_t>& cfi_info,
                 const ArrayRef<LinkerPatch>& patches = ArrayRef<LinkerPatch>());

  virtual ~CompiledMethod();

  static CompiledMethod* SwapAllocCompiledMethod(
      CompilerDriver* driver,
      InstructionSet instruction_set,
      const ArrayRef<const uint8_t>& quick_code,
      const size_t frame_size_in_bytes,
      const uint32_t core_spill_mask,
      const uint32_t fp_spill_mask,
      DefaultSrcMap* src_mapping_table,
      const ArrayRef<const uint8_t>& mapping_table,
      const ArrayRef<const uint8_t>& vmap_table,
      const ArrayRef<const uint8_t>& native_gc_map,
      const ArrayRef<const uint8_t>& cfi_info,
      const ArrayRef<LinkerPatch>& patches = ArrayRef<LinkerPatch>());

  static CompiledMethod* SwapAllocCompiledMethodStackMap(
      CompilerDriver* driver,
      InstructionSet instruction_set,
      const ArrayRef<const uint8_t>& quick_code,
      const size_t frame_size_in_bytes,
      const uint32_t core_spill_mask,
      const uint32_t fp_spill_mask,
      const ArrayRef<const uint8_t>& stack_map);

  static CompiledMethod* SwapAllocCompiledMethodCFI(CompilerDriver* driver,
                                                    InstructionSet instruction_set,
                                                    const ArrayRef<const uint8_t>& quick_code,
                                                    const size_t frame_size_in_bytes,
                                                    const uint32_t core_spill_mask,
                                                    const uint32_t fp_spill_mask,
                                                    const ArrayRef<const uint8_t>& cfi_info);

  static void ReleaseSwapAllocatedCompiledMethod(CompilerDriver* driver, CompiledMethod* m);

  size_t GetFrameSizeInBytes() const {
    return frame_size_in_bytes_;
  }

  uint32_t GetCoreSpillMask() const {
    return core_spill_mask_;
  }

  uint32_t GetFpSpillMask() const {
    return fp_spill_mask_;
  }

  const SwapSrcMap& GetSrcMappingTable() const {
    DCHECK(src_mapping_table_ != nullptr);
    return *src_mapping_table_;
  }

  SwapVector<uint8_t> const* GetMappingTable() const {
    return mapping_table_;
  }

  const SwapVector<uint8_t>* GetVmapTable() const {
    DCHECK(vmap_table_ != nullptr);
    return vmap_table_;
  }

  SwapVector<uint8_t> const* GetGcMap() const {
    return gc_map_;
  }

  const SwapVector<uint8_t>* GetCFIInfo() const {
    return cfi_info_;
  }

  const SwapVector<LinkerPatch>& GetPatches() const {
    return patches_;
  }

 private:
  // Whether or not the arrays are owned by the compiled method or dedupe sets.
  const bool owns_arrays_;
  // For quick code, the size of the activation used by the code.
  const size_t frame_size_in_bytes_;
  // For quick code, a bit mask describing spilled GPR callee-save registers.
  const uint32_t core_spill_mask_;
  // For quick code, a bit mask describing spilled FPR callee-save registers.
  const uint32_t fp_spill_mask_;
  // For quick code, a set of pairs (PC, Line) mapping from native PC offset to Java line
  SwapSrcMap* src_mapping_table_;
  // For quick code, a uleb128 encoded map from native PC offset to dex PC aswell as dex PC to
  // native PC offset. Size prefixed.
  SwapVector<uint8_t>* mapping_table_;
  // For quick code, a uleb128 encoded map from GPR/FPR register to dex register. Size prefixed.
  SwapVector<uint8_t>* vmap_table_;
  // For quick code, a map keyed by native PC indices to bitmaps describing what dalvik registers
  // are live.
  SwapVector<uint8_t>* gc_map_;
  // For quick code, a FDE entry for the debug_frame section.
  SwapVector<uint8_t>* cfi_info_;
  // For quick code, linker patches needed by the method.
  SwapVector<LinkerPatch> patches_;
};

}  // namespace art

#endif  // ART_COMPILER_COMPILED_METHOD_H_
