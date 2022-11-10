// Copyright 2022 The SiliFuzz Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./snap/snap_relocator.h"

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>

#include "./snap/snap.h"
#include "./util/mmapped_memory_ptr.h"

namespace silifuzz {

namespace {

// Convenience function to create a null corpus for errors.
MmappedMemoryPtr<const SnapCorpus> make_null_corpus() {
  return MakeMmappedMemoryPtr<const SnapCorpus>(nullptr, 0);
}

}  // namespace

// Similar to RETURN_IF_NOT_OK() but for SnapRelocator::Error.
#define RETURN_IF_RELOCATION_FAILED(exp)   \
  do {                                     \
    const Error error = (exp);             \
    if (error != Error::kOk) return error; \
  } while (0)

template <typename T>
SnapRelocator::Error SnapRelocator::ValidateRelocatedAddress(
    uintptr_t address) {
  // The whole object must be within corpus bounds.
  if (address < start_address_ || address + sizeof(T) > limit_address_)
    return Error::kOutOfBound;

  // Address be correctly aligned.
  if (address % alignof(T) != 0) return Error::kAlignment;

  return Error::kOk;
}

template <typename T>
SnapRelocator::Error SnapRelocator::AdjustPointer(T*& ptr) {
  // A pointer in a relocatable Snap corpus offset is just offset from the
  // start of the corpus. The actual run time address of the pointed object
  // is recovered by simply adding the start address of the corpus.
  uintptr_t adjusted_address =
      reinterpret_cast<uintptr_t>(ptr) + start_address_;

  RETURN_IF_RELOCATION_FAILED(ValidateRelocatedAddress<T>(adjusted_address));

  ptr = reinterpret_cast<T*>(adjusted_address);
  return Error::kOk;
}

template <typename T>
SnapRelocator::Error SnapRelocator::AdjustArray(Snap::Array<T>& array) {
  if (array.size > 0) {
    RETURN_IF_RELOCATION_FAILED(AdjustPointer(array.elements));
    // Check that the last element is within bound. The beginning of array
    // is checked already by AdjustPointer() above.
    return ValidateRelocatedAddress<T>(
        reinterpret_cast<uintptr_t>(&array.elements[array.size - 1]));
  } else {
    array.elements = nullptr;
    return Error::kOk;
  }
}

SnapRelocator::Error SnapRelocator::RelocateMemoryBytesArray(
    Snap::Array<Snap::MemoryBytes>& memory_bytes_array) {
  RETURN_IF_RELOCATION_FAILED(AdjustArray(memory_bytes_array));
  for (size_t i = 0; i < memory_bytes_array.size; ++i) {
    Snap::MemoryBytes& memory_byte =
        const_cast<Snap::MemoryBytes&>(memory_bytes_array[i]);
    if (!memory_byte.repeating()) {
      RETURN_IF_RELOCATION_FAILED(
          AdjustPointer(memory_byte.data.byte_values.elements));
    }
  }
  return Error::kOk;
}

SnapRelocator::Error SnapRelocator::RelocateCorpus() {
  // We know the pointer is in bounds, but check that the struct fits in memory
  // and is aligned.
  RETURN_IF_RELOCATION_FAILED(
      ValidateRelocatedAddress<SnapCorpus>(start_address_));

  SnapCorpus& corpus = *reinterpret_cast<SnapCorpus*>(start_address_);

  // If this constant isn't at the start of the file, it's likely not a corpus.
  if (corpus.magic != kSnapCorpusMagic) {
    return Error::kBadData;
  }
  if (corpus.corpus_type_size != sizeof(SnapCorpus)) {
    return Error::kBadData;
  }
  if (corpus.snap_type_size != sizeof(Snap)) {
    return Error::kBadData;
  }
  if (corpus.register_state_type_size != sizeof(Snap::RegisterState)) {
    return Error::kBadData;
  }

  RETURN_IF_RELOCATION_FAILED(AdjustArray(corpus.snaps));
  for (size_t i = 0; i < corpus.snaps.size; ++i) {
    // Adjust the pointer in the array.
    RETURN_IF_RELOCATION_FAILED(
        AdjustPointer(const_cast<Snap*&>(corpus.snaps[i])));

    // Adjust pointers in this Snap.
    Snap& snap = *const_cast<Snap*>(corpus.snaps[i]);
    RETURN_IF_RELOCATION_FAILED(AdjustPointer(snap.id));
    RETURN_IF_RELOCATION_FAILED(AdjustArray(snap.memory_mappings));

    // Adjust register pointers.
    RETURN_IF_RELOCATION_FAILED(AdjustPointer(snap.registers));
    RETURN_IF_RELOCATION_FAILED(AdjustPointer(snap.end_state_registers));

    // Adjust memory bytes arrays.
    RETURN_IF_RELOCATION_FAILED(RelocateMemoryBytesArray(snap.memory_bytes));
    RETURN_IF_RELOCATION_FAILED(
        RelocateMemoryBytesArray(snap.end_state_memory_bytes));
  }
  return Error::kOk;
}

// static
MmappedMemoryPtr<const SnapCorpus> SnapRelocator::RelocateCorpus(
    MmappedMemoryPtr<char> relocatable, Error* error) {
  const size_t byte_size = MmappedMemorySize(relocatable);
  if (byte_size == 0) {
    *error = Error::kEmptyCorpus;
    return make_null_corpus();
  }

  uintptr_t start_address = reinterpret_cast<uintptr_t>(relocatable.get());
  uintptr_t limit_address = start_address + byte_size;
  SnapRelocator relocator(start_address, limit_address);

  // Relocate corpus
  *error = relocator.RelocateCorpus();
  if (*error != Error::kOk) return make_null_corpus();

  // mprotect corpus after relocation.
  if (mprotect(reinterpret_cast<void*>(relocatable.get()), byte_size,
               PROT_READ) != 0) {
    *error = Error::kMprotect;
    return make_null_corpus();
  }

  auto corpus = reinterpret_cast<const SnapCorpus*>(relocatable.release());

  *error = Error::kOk;
  return MakeMmappedMemoryPtr(corpus, byte_size);
}

}  // namespace silifuzz
