/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "binary-reader.h"

#include <cassert>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "binary.h"
#include "binary-reader-logging.h"
#include "config.h"
#include "stream.h"
#include "utf8.h"

#if HAVE_ALLOCA
#include <alloca.h>
#endif

#define CHECK_RESULT(expr)  \
  do {                      \
    if (Failed(expr))       \
      return Result::Error; \
  } while (0)

#define ERROR_UNLESS(expr, ...) \
  do {                          \
    if (!(expr)) {              \
      PrintError(__VA_ARGS__);  \
      return Result::Error;     \
    }                           \
  } while (0)

#define ERROR_UNLESS_OPCODE_ENABLED(opcode)    \
  do {                                         \
    if (!opcode.IsEnabled(options_->features)) \
      return ReportUnexpectedOpcode(opcode);   \
  } while (0)

#define CALLBACK0(member)                              \
  ERROR_UNLESS(Succeeded(delegate_->member()), #member \
               " callback failed")

#define CALLBACK(member, ...)                             \
  ERROR_UNLESS(Succeeded(delegate_->member(__VA_ARGS__)), \
               #member " callback failed")

namespace wabt {

#define BYTE_AT(type, i, shift) ((static_cast<type>(p[i]) & 0x7f) << (shift))

#define LEB128_1(type) (BYTE_AT(type, 0, 0))
#define LEB128_2(type) (BYTE_AT(type, 1, 7) | LEB128_1(type))
#define LEB128_3(type) (BYTE_AT(type, 2, 14) | LEB128_2(type))
#define LEB128_4(type) (BYTE_AT(type, 3, 21) | LEB128_3(type))
#define LEB128_5(type) (BYTE_AT(type, 4, 28) | LEB128_4(type))
#define LEB128_6(type) (BYTE_AT(type, 5, 35) | LEB128_5(type))
#define LEB128_7(type) (BYTE_AT(type, 6, 42) | LEB128_6(type))
#define LEB128_8(type) (BYTE_AT(type, 7, 49) | LEB128_7(type))
#define LEB128_9(type) (BYTE_AT(type, 8, 56) | LEB128_8(type))
#define LEB128_10(type) (BYTE_AT(type, 9, 63) | LEB128_9(type))

#define SHIFT_AMOUNT(type, sign_bit) (sizeof(type) * 8 - 1 - (sign_bit))
#define SIGN_EXTEND(type, value, sign_bit)                       \
  (static_cast<type>((value) << SHIFT_AMOUNT(type, sign_bit)) >> \
   SHIFT_AMOUNT(type, sign_bit))

// TODO(binji): move LEB functions elsewhere
size_t ReadU32Leb128(const uint8_t* p,
                     const uint8_t* end,
                     uint32_t* out_value) {
  if (p < end && (p[0] & 0x80) == 0) {
    *out_value = LEB128_1(uint32_t);
    return 1;
  } else if (p + 1 < end && (p[1] & 0x80) == 0) {
    *out_value = LEB128_2(uint32_t);
    return 2;
  } else if (p + 2 < end && (p[2] & 0x80) == 0) {
    *out_value = LEB128_3(uint32_t);
    return 3;
  } else if (p + 3 < end && (p[3] & 0x80) == 0) {
    *out_value = LEB128_4(uint32_t);
    return 4;
  } else if (p + 4 < end && (p[4] & 0x80) == 0) {
    /* the top bits set represent values > 32 bits */
    if (p[4] & 0xf0)
      return 0;
    *out_value = LEB128_5(uint32_t);
    return 5;
  } else {
    /* past the end */
    *out_value = 0;
    return 0;
  }
}

size_t ReadI32Leb128(const uint8_t* p,
                     const uint8_t* end,
                     uint32_t* out_value) {
  if (p < end && (p[0] & 0x80) == 0) {
    uint32_t result = LEB128_1(uint32_t);
    *out_value = SIGN_EXTEND(int32_t, result, 6);
    return 1;
  } else if (p + 1 < end && (p[1] & 0x80) == 0) {
    uint32_t result = LEB128_2(uint32_t);
    *out_value = SIGN_EXTEND(int32_t, result, 13);
    return 2;
  } else if (p + 2 < end && (p[2] & 0x80) == 0) {
    uint32_t result = LEB128_3(uint32_t);
    *out_value = SIGN_EXTEND(int32_t, result, 20);
    return 3;
  } else if (p + 3 < end && (p[3] & 0x80) == 0) {
    uint32_t result = LEB128_4(uint32_t);
    *out_value = SIGN_EXTEND(int32_t, result, 27);
    return 4;
  } else if (p + 4 < end && (p[4] & 0x80) == 0) {
    /* the top bits should be a sign-extension of the sign bit */
    bool sign_bit_set = (p[4] & 0x8);
    int top_bits = p[4] & 0xf0;
    if ((sign_bit_set && top_bits != 0x70) ||
        (!sign_bit_set && top_bits != 0)) {
      return 0;
    }
    uint32_t result = LEB128_5(uint32_t);
    *out_value = result;
    return 5;
  } else {
    /* past the end */
    return 0;
  }
}

namespace {

class BinaryReader {
 public:
  BinaryReader(const void* data,
               size_t size,
               BinaryReaderDelegate* delegate,
               const ReadBinaryOptions* options);

  Result ReadModule();

 private:
  void WABT_PRINTF_FORMAT(2, 3) PrintError(const char* format, ...);
  Result ReadOpcode(Opcode* out_value, const char* desc) WABT_WARN_UNUSED;
  Result ReadU8(uint8_t* out_value, const char* desc) WABT_WARN_UNUSED;
  Result ReadU32(uint32_t* out_value, const char* desc) WABT_WARN_UNUSED;
  Result ReadF32(uint32_t* out_value, const char* desc) WABT_WARN_UNUSED;
  Result ReadF64(uint64_t* out_value, const char* desc) WABT_WARN_UNUSED;
  Result ReadU32Leb128(uint32_t* out_value, const char* desc) WABT_WARN_UNUSED;
  Result ReadI32Leb128(uint32_t* out_value, const char* desc) WABT_WARN_UNUSED;
  Result ReadI64Leb128(uint64_t* out_value, const char* desc) WABT_WARN_UNUSED;
  Result ReadType(Type* out_value, const char* desc) WABT_WARN_UNUSED;
  Result ReadStr(string_view* out_str, const char* desc) WABT_WARN_UNUSED;
  Result ReadBytes(const void** out_data,
                   Address* out_data_size,
                   const char* desc) WABT_WARN_UNUSED;
  Result ReadIndex(Index* index, const char* desc) WABT_WARN_UNUSED;
  Result ReadOffset(Offset* offset, const char* desc) WABT_WARN_UNUSED;

  Index NumTotalFuncs();
  Index NumTotalTables();
  Index NumTotalMemories();
  Index NumTotalGlobals();

  Result ReadInitExpr(Index index) WABT_WARN_UNUSED;
  Result ReadTable(Type* out_elem_type,
                   Limits* out_elem_limits) WABT_WARN_UNUSED;
  Result ReadMemory(Limits* out_page_limits) WABT_WARN_UNUSED;
  Result ReadGlobalHeader(Type* out_type, bool* out_mutable) WABT_WARN_UNUSED;
  Result ReadExceptionType(TypeVector& sig) WABT_WARN_UNUSED;
  Result ReadFunctionBody(Offset end_offset) WABT_WARN_UNUSED;
  Result ReadNamesSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadRelocSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadLinkingSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadCustomSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadTypeSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadImportSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadFunctionSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadTableSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadMemorySection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadGlobalSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadExportSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadStartSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadElemSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadCodeSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadDataSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadExceptionSection(Offset section_size) WABT_WARN_UNUSED;
  Result ReadSections() WABT_WARN_UNUSED;
  Result ReportUnexpectedOpcode(Opcode opcode, const char* message = nullptr);

  size_t read_end_ = 0; /* Either the section end or data_size. */
  BinaryReaderDelegate::State state_;
  BinaryReaderLogging logging_delegate_;
  BinaryReaderDelegate* delegate_ = nullptr;
  TypeVector param_types_;
  std::vector<Index> target_depths_;
  const ReadBinaryOptions* options_ = nullptr;
  BinarySection last_known_section_ = BinarySection::Invalid;
  Index num_signatures_ = 0;
  Index num_imports_ = 0;
  Index num_func_imports_ = 0;
  Index num_table_imports_ = 0;
  Index num_memory_imports_ = 0;
  Index num_global_imports_ = 0;
  Index num_exception_imports_ = 0;
  Index num_function_signatures_ = 0;
  Index num_tables_ = 0;
  Index num_memories_ = 0;
  Index num_globals_ = 0;
  Index num_exports_ = 0;
  Index num_function_bodies_ = 0;
  Index num_exceptions_ = 0;
};

BinaryReader::BinaryReader(const void* data,
                           size_t size,
                           BinaryReaderDelegate* delegate,
                           const ReadBinaryOptions* options)
    : read_end_(size),
      state_(static_cast<const uint8_t*>(data), size),
      logging_delegate_(options->log_stream, delegate),
      delegate_(options->log_stream ? &logging_delegate_ : delegate),
      options_(options),
      last_known_section_(BinarySection::Invalid) {
  delegate->OnSetState(&state_);
}

void WABT_PRINTF_FORMAT(2, 3) BinaryReader::PrintError(const char* format,
                                                       ...) {
  WABT_SNPRINTF_ALLOCA(buffer, length, format);
  bool handled = delegate_->OnError(buffer);

  if (!handled) {
    /* Not great to just print, but we don't want to eat the error either. */
    fprintf(stderr, "*ERROR*: @0x%08zx: %s\n", state_.offset, buffer);
  }
}

#define IN_SIZE(type)                                           \
  if (state_.offset + sizeof(type) > read_end_) {               \
    PrintError("unable to read " #type ": %s", desc);           \
    return Result::Error;                                       \
  }                                                             \
  memcpy(out_value, state_.data + state_.offset, sizeof(type)); \
  state_.offset += sizeof(type);                                \
  return Result::Ok

Result BinaryReader::ReportUnexpectedOpcode(Opcode opcode,
                                            const char* message) {
  const char* maybe_space = " ";
  if (!message)
    message = maybe_space = "";
  if (opcode.HasPrefix()) {
    PrintError("unexpected opcode%s%s: %d %d (0x%x 0x%x)", maybe_space, message,
               opcode.GetPrefix(), opcode.GetCode(), opcode.GetPrefix(),
               opcode.GetCode());
  } else {
    PrintError("unexpected opcode%s%s: %d (0x%x)",
               maybe_space, message, opcode.GetCode(), opcode.GetCode());
  }
  return Result::Error;
}

Result BinaryReader::ReadOpcode(Opcode* out_value, const char* desc) {
  uint8_t value = 0;
  CHECK_RESULT(ReadU8(&value, desc));

  if (Opcode::IsPrefixByte(value)) {
    uint32_t code;
    CHECK_RESULT(ReadU32Leb128(&code, desc));
    *out_value = Opcode::FromCode(value, code);
  } else {
    *out_value = Opcode::FromCode(value);
  }
  return Result::Ok;
}

Result BinaryReader::ReadU8(uint8_t* out_value, const char* desc) {
  IN_SIZE(uint8_t);
}

Result BinaryReader::ReadU32(uint32_t* out_value, const char* desc) {
  IN_SIZE(uint32_t);
}

Result BinaryReader::ReadF32(uint32_t* out_value, const char* desc) {
  IN_SIZE(float);
}

Result BinaryReader::ReadF64(uint64_t* out_value, const char* desc) {
  IN_SIZE(double);
}

#undef IN_SIZE

Result BinaryReader::ReadU32Leb128(uint32_t* out_value, const char* desc) {
  const uint8_t* p = state_.data + state_.offset;
  const uint8_t* end = state_.data + read_end_;
  size_t bytes_read = wabt::ReadU32Leb128(p, end, out_value);
  ERROR_UNLESS(bytes_read > 0, "unable to read u32 leb128: %s", desc);
  state_.offset += bytes_read;
  return Result::Ok;
}

Result BinaryReader::ReadI32Leb128(uint32_t* out_value, const char* desc) {
  const uint8_t* p = state_.data + state_.offset;
  const uint8_t* end = state_.data + read_end_;
  size_t bytes_read = wabt::ReadI32Leb128(p, end, out_value);
  ERROR_UNLESS(bytes_read > 0, "unable to read i32 leb128: %s", desc);
  state_.offset += bytes_read;
  return Result::Ok;
}

Result BinaryReader::ReadI64Leb128(uint64_t* out_value, const char* desc) {
  const uint8_t* p = state_.data + state_.offset;
  const uint8_t* end = state_.data + read_end_;

  if (p < end && (p[0] & 0x80) == 0) {
    uint64_t result = LEB128_1(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 6);
    state_.offset += 1;
  } else if (p + 1 < end && (p[1] & 0x80) == 0) {
    uint64_t result = LEB128_2(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 13);
    state_.offset += 2;
  } else if (p + 2 < end && (p[2] & 0x80) == 0) {
    uint64_t result = LEB128_3(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 20);
    state_.offset += 3;
  } else if (p + 3 < end && (p[3] & 0x80) == 0) {
    uint64_t result = LEB128_4(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 27);
    state_.offset += 4;
  } else if (p + 4 < end && (p[4] & 0x80) == 0) {
    uint64_t result = LEB128_5(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 34);
    state_.offset += 5;
  } else if (p + 5 < end && (p[5] & 0x80) == 0) {
    uint64_t result = LEB128_6(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 41);
    state_.offset += 6;
  } else if (p + 6 < end && (p[6] & 0x80) == 0) {
    uint64_t result = LEB128_7(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 48);
    state_.offset += 7;
  } else if (p + 7 < end && (p[7] & 0x80) == 0) {
    uint64_t result = LEB128_8(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 55);
    state_.offset += 8;
  } else if (p + 8 < end && (p[8] & 0x80) == 0) {
    uint64_t result = LEB128_9(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 62);
    state_.offset += 9;
  } else if (p + 9 < end && (p[9] & 0x80) == 0) {
    /* the top bits should be a sign-extension of the sign bit */
    bool sign_bit_set = (p[9] & 0x1);
    int top_bits = p[9] & 0xfe;
    if ((sign_bit_set && top_bits != 0x7e) ||
        (!sign_bit_set && top_bits != 0)) {
      PrintError("invalid i64 leb128: %s", desc);
      return Result::Error;
    }
    uint64_t result = LEB128_10(uint64_t);
    *out_value = result;
    state_.offset += 10;
  } else {
    /* past the end */
    PrintError("unable to read i64 leb128: %s", desc);
    return Result::Error;
  }
  return Result::Ok;
}

#undef BYTE_AT
#undef LEB128_1
#undef LEB128_2
#undef LEB128_3
#undef LEB128_4
#undef LEB128_5
#undef LEB128_6
#undef LEB128_7
#undef LEB128_8
#undef LEB128_9
#undef LEB128_10
#undef SHIFT_AMOUNT
#undef SIGN_EXTEND

Result BinaryReader::ReadType(Type* out_value, const char* desc) {
  uint32_t type = 0;
  CHECK_RESULT(ReadI32Leb128(&type, desc));
  /* Must be in the vs7 range: [-128, 127). */
  ERROR_UNLESS(
      static_cast<int32_t>(type) >= -128 && static_cast<int32_t>(type) <= 127,
      "invalid type: %d", type);
  *out_value = static_cast<Type>(type);
  return Result::Ok;
}

Result BinaryReader::ReadStr(string_view* out_str, const char* desc) {
  uint32_t str_len = 0;
  CHECK_RESULT(ReadU32Leb128(&str_len, "string length"));

  ERROR_UNLESS(state_.offset + str_len <= read_end_,
               "unable to read string: %s", desc);

  *out_str = string_view(
      reinterpret_cast<const char*>(state_.data) + state_.offset, str_len);
  state_.offset += str_len;

  ERROR_UNLESS(IsValidUtf8(out_str->data(), out_str->length()),
               "invalid utf-8 encoding: %s", desc);
  return Result::Ok;
}

Result BinaryReader::ReadBytes(const void** out_data,
                               Address* out_data_size,
                               const char* desc) {
  uint32_t data_size = 0;
  CHECK_RESULT(ReadU32Leb128(&data_size, "data size"));

  ERROR_UNLESS(state_.offset + data_size <= read_end_,
               "unable to read data: %s", desc);

  *out_data = static_cast<const uint8_t*>(state_.data) + state_.offset;
  *out_data_size = data_size;
  state_.offset += data_size;
  return Result::Ok;
}

Result BinaryReader::ReadIndex(Index* index, const char* desc) {
  uint32_t value;
  CHECK_RESULT(ReadU32Leb128(&value, desc));
  *index = value;
  return Result::Ok;
}

Result BinaryReader::ReadOffset(Offset* offset, const char* desc) {
  uint32_t value;
  CHECK_RESULT(ReadU32Leb128(&value, desc));
  *offset = value;
  return Result::Ok;
}

static bool is_valid_external_kind(uint8_t kind) {
  return kind < kExternalKindCount;
}

static bool is_concrete_type(Type type) {
  switch (type) {
    case Type::I32:
    case Type::I64:
    case Type::F32:
    case Type::F64:
      return true;

    default:
      return false;
  }
}

static bool is_inline_sig_type(Type type) {
  return is_concrete_type(type) || type == Type::Void;
}

Index BinaryReader::NumTotalFuncs() {
  return num_func_imports_ + num_function_signatures_;
}

Index BinaryReader::NumTotalTables() {
  return num_table_imports_ + num_tables_;
}

Index BinaryReader::NumTotalMemories() {
  return num_memory_imports_ + num_memories_;
}

Index BinaryReader::NumTotalGlobals() {
  return num_global_imports_ + num_globals_;
}

Result BinaryReader::ReadInitExpr(Index index) {
  Opcode opcode;
  CHECK_RESULT(ReadOpcode(&opcode, "opcode"));
  switch (opcode) {
    case Opcode::I32Const: {
      uint32_t value = 0;
      CHECK_RESULT(ReadI32Leb128(&value, "init_expr i32.const value"));
      CALLBACK(OnInitExprI32ConstExpr, index, value);
      break;
    }

    case Opcode::I64Const: {
      uint64_t value = 0;
      CHECK_RESULT(ReadI64Leb128(&value, "init_expr i64.const value"));
      CALLBACK(OnInitExprI64ConstExpr, index, value);
      break;
    }

    case Opcode::F32Const: {
      uint32_t value_bits = 0;
      CHECK_RESULT(ReadF32(&value_bits, "init_expr f32.const value"));
      CALLBACK(OnInitExprF32ConstExpr, index, value_bits);
      break;
    }

    case Opcode::F64Const: {
      uint64_t value_bits = 0;
      CHECK_RESULT(ReadF64(&value_bits, "init_expr f64.const value"));
      CALLBACK(OnInitExprF64ConstExpr, index, value_bits);
      break;
    }

    case Opcode::GetGlobal: {
      Index global_index;
      CHECK_RESULT(ReadIndex(&global_index, "init_expr get_global index"));
      CALLBACK(OnInitExprGetGlobalExpr, index, global_index);
      break;
    }

    case Opcode::End:
      return Result::Ok;

    default:
      return ReportUnexpectedOpcode(opcode, "in initializer expression");
  }

  CHECK_RESULT(ReadOpcode(&opcode, "opcode"));
  ERROR_UNLESS(opcode == Opcode::End,
               "expected END opcode after initializer expression");
  return Result::Ok;
}

Result BinaryReader::ReadTable(Type* out_elem_type, Limits* out_elem_limits) {
  CHECK_RESULT(ReadType(out_elem_type, "table elem type"));
  ERROR_UNLESS(*out_elem_type == Type::Anyfunc,
               "table elem type must by anyfunc");

  uint32_t flags;
  uint32_t initial;
  uint32_t max = 0;
  CHECK_RESULT(ReadU32Leb128(&flags, "table flags"));
  CHECK_RESULT(ReadU32Leb128(&initial, "table initial elem count"));
  bool has_max = flags & WABT_BINARY_LIMITS_HAS_MAX_FLAG;
  if (has_max) {
    CHECK_RESULT(ReadU32Leb128(&max, "table max elem count"));
    ERROR_UNLESS(initial <= max,
                 "table initial elem count must be <= max elem count");
  }

  out_elem_limits->has_max = has_max;
  out_elem_limits->initial = initial;
  out_elem_limits->max = max;
  return Result::Ok;
}

Result BinaryReader::ReadMemory(Limits* out_page_limits) {
  uint32_t flags;
  uint32_t initial;
  uint32_t max = 0;
  CHECK_RESULT(ReadU32Leb128(&flags, "memory flags"));
  CHECK_RESULT(ReadU32Leb128(&initial, "memory initial page count"));
  bool has_max = flags & WABT_BINARY_LIMITS_HAS_MAX_FLAG;
  ERROR_UNLESS(initial <= WABT_MAX_PAGES, "invalid memory initial size");
  if (has_max) {
    CHECK_RESULT(ReadU32Leb128(&max, "memory max page count"));
    ERROR_UNLESS(max <= WABT_MAX_PAGES, "invalid memory max size");
    ERROR_UNLESS(initial <= max, "memory initial size must be <= max size");
  }

  out_page_limits->has_max = has_max;
  out_page_limits->initial = initial;
  out_page_limits->max = max;
  return Result::Ok;
}

Result BinaryReader::ReadGlobalHeader(Type* out_type, bool* out_mutable) {
  Type global_type = Type::Void;
  uint8_t mutable_ = 0;
  CHECK_RESULT(ReadType(&global_type, "global type"));
  ERROR_UNLESS(is_concrete_type(global_type), "invalid global type: %#x",
               static_cast<int>(global_type));

  CHECK_RESULT(ReadU8(&mutable_, "global mutability"));
  ERROR_UNLESS(mutable_ <= 1, "global mutability must be 0 or 1");

  *out_type = global_type;
  *out_mutable = mutable_;
  return Result::Ok;
}

Result BinaryReader::ReadFunctionBody(Offset end_offset) {
  bool seen_end_opcode = false;
  while (state_.offset < end_offset) {
    Opcode opcode;
    CHECK_RESULT(ReadOpcode(&opcode, "opcode"));
    CALLBACK(OnOpcode, opcode);
    switch (opcode) {
      case Opcode::Unreachable:
        CALLBACK0(OnUnreachableExpr);
        CALLBACK0(OnOpcodeBare);
        break;

      case Opcode::Block: {
        Type sig_type;
        CHECK_RESULT(ReadType(&sig_type, "block signature type"));
        ERROR_UNLESS(is_inline_sig_type(sig_type),
                     "expected valid block signature type");
        Index num_types = sig_type == Type::Void ? 0 : 1;
        CALLBACK(OnBlockExpr, num_types, &sig_type);
        CALLBACK(OnOpcodeBlockSig, num_types, &sig_type);
        break;
      }

      case Opcode::Loop: {
        Type sig_type;
        CHECK_RESULT(ReadType(&sig_type, "loop signature type"));
        ERROR_UNLESS(is_inline_sig_type(sig_type),
                     "expected valid block signature type");
        Index num_types = sig_type == Type::Void ? 0 : 1;
        CALLBACK(OnLoopExpr, num_types, &sig_type);
        CALLBACK(OnOpcodeBlockSig, num_types, &sig_type);
        break;
      }

      case Opcode::If: {
        Type sig_type;
        CHECK_RESULT(ReadType(&sig_type, "if signature type"));
        ERROR_UNLESS(is_inline_sig_type(sig_type),
                     "expected valid block signature type");
        Index num_types = sig_type == Type::Void ? 0 : 1;
        CALLBACK(OnIfExpr, num_types, &sig_type);
        CALLBACK(OnOpcodeBlockSig, num_types, &sig_type);
        break;
      }

      case Opcode::Else:
        CALLBACK0(OnElseExpr);
        CALLBACK0(OnOpcodeBare);
        break;

      case Opcode::Select:
        CALLBACK0(OnSelectExpr);
        CALLBACK0(OnOpcodeBare);
        break;

      case Opcode::Br: {
        Index depth;
        CHECK_RESULT(ReadIndex(&depth, "br depth"));
        CALLBACK(OnBrExpr, depth);
        CALLBACK(OnOpcodeIndex, depth);
        break;
      }

      case Opcode::BrIf: {
        Index depth;
        CHECK_RESULT(ReadIndex(&depth, "br_if depth"));
        CALLBACK(OnBrIfExpr, depth);
        CALLBACK(OnOpcodeIndex, depth);
        break;
      }

      case Opcode::BrTable: {
        Index num_targets;
        CHECK_RESULT(ReadIndex(&num_targets, "br_table target count"));
        target_depths_.resize(num_targets);

        for (Index i = 0; i < num_targets; ++i) {
          Index target_depth;
          CHECK_RESULT(ReadIndex(&target_depth, "br_table target depth"));
          target_depths_[i] = target_depth;
        }

        Index default_target_depth;
        CHECK_RESULT(
            ReadIndex(&default_target_depth, "br_table default target depth"));

        Index* target_depths = num_targets ? target_depths_.data() : nullptr;

        CALLBACK(OnBrTableExpr, num_targets, target_depths,
                 default_target_depth);
        break;
      }

      case Opcode::Return:
        CALLBACK0(OnReturnExpr);
        CALLBACK0(OnOpcodeBare);
        break;

      case Opcode::Nop:
        CALLBACK0(OnNopExpr);
        CALLBACK0(OnOpcodeBare);
        break;

      case Opcode::Drop:
        CALLBACK0(OnDropExpr);
        CALLBACK0(OnOpcodeBare);
        break;

      case Opcode::End:
        if (state_.offset == end_offset) {
          seen_end_opcode = true;
          CALLBACK0(OnEndFunc);
        } else {
          CALLBACK0(OnEndExpr);
        }
        break;

      case Opcode::I32Const: {
        uint32_t value;
        CHECK_RESULT(ReadI32Leb128(&value, "i32.const value"));
        CALLBACK(OnI32ConstExpr, value);
        CALLBACK(OnOpcodeUint32, value);
        break;
      }

      case Opcode::I64Const: {
        uint64_t value;
        CHECK_RESULT(ReadI64Leb128(&value, "i64.const value"));
        CALLBACK(OnI64ConstExpr, value);
        CALLBACK(OnOpcodeUint64, value);
        break;
      }

      case Opcode::F32Const: {
        uint32_t value_bits = 0;
        CHECK_RESULT(ReadF32(&value_bits, "f32.const value"));
        CALLBACK(OnF32ConstExpr, value_bits);
        CALLBACK(OnOpcodeF32, value_bits);
        break;
      }

      case Opcode::F64Const: {
        uint64_t value_bits = 0;
        CHECK_RESULT(ReadF64(&value_bits, "f64.const value"));
        CALLBACK(OnF64ConstExpr, value_bits);
        CALLBACK(OnOpcodeF64, value_bits);
        break;
      }

      case Opcode::GetGlobal: {
        Index global_index;
        CHECK_RESULT(ReadIndex(&global_index, "get_global global index"));
        CALLBACK(OnGetGlobalExpr, global_index);
        CALLBACK(OnOpcodeIndex, global_index);
        break;
      }

      case Opcode::GetLocal: {
        Index local_index;
        CHECK_RESULT(ReadIndex(&local_index, "get_local local index"));
        CALLBACK(OnGetLocalExpr, local_index);
        CALLBACK(OnOpcodeIndex, local_index);
        break;
      }

      case Opcode::SetGlobal: {
        Index global_index;
        CHECK_RESULT(ReadIndex(&global_index, "set_global global index"));
        CALLBACK(OnSetGlobalExpr, global_index);
        CALLBACK(OnOpcodeIndex, global_index);
        break;
      }

      case Opcode::SetLocal: {
        Index local_index;
        CHECK_RESULT(ReadIndex(&local_index, "set_local local index"));
        CALLBACK(OnSetLocalExpr, local_index);
        CALLBACK(OnOpcodeIndex, local_index);
        break;
      }

      case Opcode::Call: {
        Index func_index;
        CHECK_RESULT(ReadIndex(&func_index, "call function index"));
        ERROR_UNLESS(func_index < NumTotalFuncs(),
                     "invalid call function index: %" PRIindex, func_index);
        CALLBACK(OnCallExpr, func_index);
        CALLBACK(OnOpcodeIndex, func_index);
        break;
      }

      case Opcode::CallIndirect: {
        Index sig_index;
        CHECK_RESULT(ReadIndex(&sig_index, "call_indirect signature index"));
        ERROR_UNLESS(sig_index < num_signatures_,
                     "invalid call_indirect signature index");
        uint32_t reserved;
        CHECK_RESULT(ReadU32Leb128(&reserved, "call_indirect reserved"));
        ERROR_UNLESS(reserved == 0, "call_indirect reserved value must be 0");
        CALLBACK(OnCallIndirectExpr, sig_index);
        CALLBACK(OnOpcodeUint32Uint32, sig_index, reserved);
        break;
      }

      case Opcode::TeeLocal: {
        Index local_index;
        CHECK_RESULT(ReadIndex(&local_index, "tee_local local index"));
        CALLBACK(OnTeeLocalExpr, local_index);
        CALLBACK(OnOpcodeIndex, local_index);
        break;
      }

      case Opcode::I32Load8S:
      case Opcode::I32Load8U:
      case Opcode::I32Load16S:
      case Opcode::I32Load16U:
      case Opcode::I64Load8S:
      case Opcode::I64Load8U:
      case Opcode::I64Load16S:
      case Opcode::I64Load16U:
      case Opcode::I64Load32S:
      case Opcode::I64Load32U:
      case Opcode::I32Load:
      case Opcode::I64Load:
      case Opcode::F32Load:
      case Opcode::F64Load: {
        uint32_t alignment_log2;
        CHECK_RESULT(ReadU32Leb128(&alignment_log2, "load alignment"));
        Address offset;
        CHECK_RESULT(ReadU32Leb128(&offset, "load offset"));

        CALLBACK(OnLoadExpr, opcode, alignment_log2, offset);
        CALLBACK(OnOpcodeUint32Uint32, alignment_log2, offset);
        break;
      }

      case Opcode::I32Store8:
      case Opcode::I32Store16:
      case Opcode::I64Store8:
      case Opcode::I64Store16:
      case Opcode::I64Store32:
      case Opcode::I32Store:
      case Opcode::I64Store:
      case Opcode::F32Store:
      case Opcode::F64Store: {
        uint32_t alignment_log2;
        CHECK_RESULT(ReadU32Leb128(&alignment_log2, "store alignment"));
        Address offset;
        CHECK_RESULT(ReadU32Leb128(&offset, "store offset"));

        CALLBACK(OnStoreExpr, opcode, alignment_log2, offset);
        CALLBACK(OnOpcodeUint32Uint32, alignment_log2, offset);
        break;
      }

      case Opcode::CurrentMemory: {
        uint32_t reserved;
        CHECK_RESULT(ReadU32Leb128(&reserved, "current_memory reserved"));
        ERROR_UNLESS(reserved == 0, "current_memory reserved value must be 0");
        CALLBACK0(OnCurrentMemoryExpr);
        CALLBACK(OnOpcodeUint32, reserved);
        break;
      }

      case Opcode::GrowMemory: {
        uint32_t reserved;
        CHECK_RESULT(ReadU32Leb128(&reserved, "grow_memory reserved"));
        ERROR_UNLESS(reserved == 0, "grow_memory reserved value must be 0");
        CALLBACK0(OnGrowMemoryExpr);
        CALLBACK(OnOpcodeUint32, reserved);
        break;
      }

      case Opcode::I32Add:
      case Opcode::I32Sub:
      case Opcode::I32Mul:
      case Opcode::I32DivS:
      case Opcode::I32DivU:
      case Opcode::I32RemS:
      case Opcode::I32RemU:
      case Opcode::I32And:
      case Opcode::I32Or:
      case Opcode::I32Xor:
      case Opcode::I32Shl:
      case Opcode::I32ShrU:
      case Opcode::I32ShrS:
      case Opcode::I32Rotr:
      case Opcode::I32Rotl:
      case Opcode::I64Add:
      case Opcode::I64Sub:
      case Opcode::I64Mul:
      case Opcode::I64DivS:
      case Opcode::I64DivU:
      case Opcode::I64RemS:
      case Opcode::I64RemU:
      case Opcode::I64And:
      case Opcode::I64Or:
      case Opcode::I64Xor:
      case Opcode::I64Shl:
      case Opcode::I64ShrU:
      case Opcode::I64ShrS:
      case Opcode::I64Rotr:
      case Opcode::I64Rotl:
      case Opcode::F32Add:
      case Opcode::F32Sub:
      case Opcode::F32Mul:
      case Opcode::F32Div:
      case Opcode::F32Min:
      case Opcode::F32Max:
      case Opcode::F32Copysign:
      case Opcode::F64Add:
      case Opcode::F64Sub:
      case Opcode::F64Mul:
      case Opcode::F64Div:
      case Opcode::F64Min:
      case Opcode::F64Max:
      case Opcode::F64Copysign:
        CALLBACK(OnBinaryExpr, opcode);
        CALLBACK0(OnOpcodeBare);
        break;

      case Opcode::I32Eq:
      case Opcode::I32Ne:
      case Opcode::I32LtS:
      case Opcode::I32LeS:
      case Opcode::I32LtU:
      case Opcode::I32LeU:
      case Opcode::I32GtS:
      case Opcode::I32GeS:
      case Opcode::I32GtU:
      case Opcode::I32GeU:
      case Opcode::I64Eq:
      case Opcode::I64Ne:
      case Opcode::I64LtS:
      case Opcode::I64LeS:
      case Opcode::I64LtU:
      case Opcode::I64LeU:
      case Opcode::I64GtS:
      case Opcode::I64GeS:
      case Opcode::I64GtU:
      case Opcode::I64GeU:
      case Opcode::F32Eq:
      case Opcode::F32Ne:
      case Opcode::F32Lt:
      case Opcode::F32Le:
      case Opcode::F32Gt:
      case Opcode::F32Ge:
      case Opcode::F64Eq:
      case Opcode::F64Ne:
      case Opcode::F64Lt:
      case Opcode::F64Le:
      case Opcode::F64Gt:
      case Opcode::F64Ge:
        CALLBACK(OnCompareExpr, opcode);
        CALLBACK0(OnOpcodeBare);
        break;

      case Opcode::I32Clz:
      case Opcode::I32Ctz:
      case Opcode::I32Popcnt:
      case Opcode::I64Clz:
      case Opcode::I64Ctz:
      case Opcode::I64Popcnt:
      case Opcode::F32Abs:
      case Opcode::F32Neg:
      case Opcode::F32Ceil:
      case Opcode::F32Floor:
      case Opcode::F32Trunc:
      case Opcode::F32Nearest:
      case Opcode::F32Sqrt:
      case Opcode::F64Abs:
      case Opcode::F64Neg:
      case Opcode::F64Ceil:
      case Opcode::F64Floor:
      case Opcode::F64Trunc:
      case Opcode::F64Nearest:
      case Opcode::F64Sqrt:
        CALLBACK(OnUnaryExpr, opcode);
        CALLBACK0(OnOpcodeBare);
        break;

      case Opcode::I32TruncSF32:
      case Opcode::I32TruncSF64:
      case Opcode::I32TruncUF32:
      case Opcode::I32TruncUF64:
      case Opcode::I32WrapI64:
      case Opcode::I64TruncSF32:
      case Opcode::I64TruncSF64:
      case Opcode::I64TruncUF32:
      case Opcode::I64TruncUF64:
      case Opcode::I64ExtendSI32:
      case Opcode::I64ExtendUI32:
      case Opcode::F32ConvertSI32:
      case Opcode::F32ConvertUI32:
      case Opcode::F32ConvertSI64:
      case Opcode::F32ConvertUI64:
      case Opcode::F32DemoteF64:
      case Opcode::F32ReinterpretI32:
      case Opcode::F64ConvertSI32:
      case Opcode::F64ConvertUI32:
      case Opcode::F64ConvertSI64:
      case Opcode::F64ConvertUI64:
      case Opcode::F64PromoteF32:
      case Opcode::F64ReinterpretI64:
      case Opcode::I32ReinterpretF32:
      case Opcode::I64ReinterpretF64:
      case Opcode::I32Eqz:
      case Opcode::I64Eqz:
        CALLBACK(OnConvertExpr, opcode);
        CALLBACK0(OnOpcodeBare);
        break;

      case Opcode::Try: {
        ERROR_UNLESS_OPCODE_ENABLED(opcode);
        Type sig_type;
        CHECK_RESULT(ReadType(&sig_type, "try signature type"));
        ERROR_UNLESS(is_inline_sig_type(sig_type),
                     "expected valid block signature type");
        Index num_types = sig_type == Type::Void ? 0 : 1;
        CALLBACK(OnTryExpr, num_types, &sig_type);
        CALLBACK(OnOpcodeBlockSig, num_types, &sig_type);
        break;
      }

      case Opcode::Catch: {
        ERROR_UNLESS_OPCODE_ENABLED(opcode);
        Index index;
        CHECK_RESULT(ReadIndex(&index, "exception index"));
        CALLBACK(OnCatchExpr, index);
        CALLBACK(OnOpcodeIndex, index);
        break;
      }

      case Opcode::CatchAll: {
        ERROR_UNLESS_OPCODE_ENABLED(opcode);
        CALLBACK(OnCatchAllExpr);
        CALLBACK0(OnOpcodeBare);
        break;
      }

      case Opcode::Rethrow: {
        ERROR_UNLESS_OPCODE_ENABLED(opcode);
        Index depth;
        CHECK_RESULT(ReadIndex(&depth, "catch depth"));
        CALLBACK(OnRethrowExpr, depth);
        CALLBACK(OnOpcodeIndex, depth);
        break;
      }

      case Opcode::Throw: {
        ERROR_UNLESS_OPCODE_ENABLED(opcode);
        Index index;
        CHECK_RESULT(ReadIndex(&index, "exception index"));
        CALLBACK(OnThrowExpr, index);
        CALLBACK(OnOpcodeIndex, index);
        break;
      }

      case Opcode::I32TruncSSatF32:
      case Opcode::I32TruncUSatF32:
      case Opcode::I32TruncSSatF64:
      case Opcode::I32TruncUSatF64:
      case Opcode::I64TruncSSatF32:
      case Opcode::I64TruncUSatF32:
      case Opcode::I64TruncSSatF64:
      case Opcode::I64TruncUSatF64:
        ERROR_UNLESS_OPCODE_ENABLED(opcode);
        CALLBACK(OnConvertExpr, opcode);
        CALLBACK0(OnOpcodeBare);
        break;

      default:
        return ReportUnexpectedOpcode(opcode);
    }
  }
  ERROR_UNLESS(state_.offset == end_offset,
               "function body longer than given size");
  ERROR_UNLESS(seen_end_opcode, "function body must end with END opcode");
  return Result::Ok;
}

Result BinaryReader::ReadNamesSection(Offset section_size) {
  CALLBACK(BeginNamesSection, section_size);
  Index i = 0;
  Offset previous_read_end = read_end_;
  uint32_t previous_subsection_type = 0;
  while (state_.offset < read_end_) {
    uint32_t name_type;
    Offset subsection_size;
    CHECK_RESULT(ReadU32Leb128(&name_type, "name type"));
    if (i != 0) {
      ERROR_UNLESS(name_type != previous_subsection_type,
                   "duplicate sub-section");
      ERROR_UNLESS(name_type >= previous_subsection_type,
                   "out-of-order sub-section");
    }
    previous_subsection_type = name_type;
    CHECK_RESULT(ReadOffset(&subsection_size, "subsection size"));
    size_t subsection_end = state_.offset + subsection_size;
    ERROR_UNLESS(subsection_end <= read_end_,
                 "invalid sub-section size: extends past end");
    read_end_ = subsection_end;

    switch (static_cast<NameSectionSubsection>(name_type)) {
      case NameSectionSubsection::Function:
        CALLBACK(OnFunctionNameSubsection, i, name_type, subsection_size);
        if (subsection_size) {
          Index num_names;
          CHECK_RESULT(ReadIndex(&num_names, "name count"));
          CALLBACK(OnFunctionNamesCount, num_names);
          Index last_function_index = kInvalidIndex;

          for (Index j = 0; j < num_names; ++j) {
            Index function_index;
            string_view function_name;

            CHECK_RESULT(ReadIndex(&function_index, "function index"));
            ERROR_UNLESS(function_index != last_function_index,
                         "duplicate function name: %u", function_index);
            ERROR_UNLESS(last_function_index == kInvalidIndex ||
                             function_index > last_function_index,
                         "function index out of order: %u", function_index);
            last_function_index = function_index;
            ERROR_UNLESS(function_index < NumTotalFuncs(),
                         "invalid function index: %" PRIindex, function_index);
            CHECK_RESULT(ReadStr(&function_name, "function name"));
            CALLBACK(OnFunctionName, function_index, function_name);
          }
        }
        break;
      case NameSectionSubsection::Local:
        CALLBACK(OnLocalNameSubsection, i, name_type, subsection_size);
        if (subsection_size) {
          Index num_funcs;
          CHECK_RESULT(ReadIndex(&num_funcs, "function count"));
          CALLBACK(OnLocalNameFunctionCount, num_funcs);
          Index last_function_index = kInvalidIndex;
          for (Index j = 0; j < num_funcs; ++j) {
            Index function_index;
            CHECK_RESULT(ReadIndex(&function_index, "function index"));
            ERROR_UNLESS(function_index < NumTotalFuncs(),
                         "invalid function index: %u", function_index);
            ERROR_UNLESS(last_function_index == kInvalidIndex ||
                             function_index > last_function_index,
                         "locals function index out of order: %u",
                         function_index);
            last_function_index = function_index;
            Index num_locals;
            CHECK_RESULT(ReadIndex(&num_locals, "local count"));
            CALLBACK(OnLocalNameLocalCount, function_index, num_locals);
            Index last_local_index = kInvalidIndex;
            for (Index k = 0; k < num_locals; ++k) {
              Index local_index;
              string_view local_name;

              CHECK_RESULT(ReadIndex(&local_index, "named index"));
              ERROR_UNLESS(local_index != last_local_index,
                           "duplicate local index: %u", local_index);
              ERROR_UNLESS(last_local_index == kInvalidIndex ||
                               local_index > last_local_index,
                           "local index out of order: %u", local_index);
              last_local_index = local_index;
              CHECK_RESULT(ReadStr(&local_name, "name"));
              CALLBACK(OnLocalName, function_index, local_index, local_name);
            }
          }
        }
        break;
      default:
        /* unknown subsection, skip it */
        state_.offset = subsection_end;
        break;
    }
    ++i;
    ERROR_UNLESS(state_.offset == subsection_end,
                 "unfinished sub-section (expected end: 0x%" PRIzx ")",
                 subsection_end);
    read_end_ = previous_read_end;
  }
  CALLBACK0(EndNamesSection);
  return Result::Ok;
}

Result BinaryReader::ReadRelocSection(Offset section_size) {
  CALLBACK(BeginRelocSection, section_size);
  uint32_t section;
  CHECK_RESULT(ReadU32Leb128(&section, "section"));
  string_view section_name;
  if (static_cast<BinarySection>(section) == BinarySection::Custom)
    CHECK_RESULT(ReadStr(&section_name, "section name"));
  Index num_relocs;
  CHECK_RESULT(ReadIndex(&num_relocs, "relocation count"));
  CALLBACK(OnRelocCount, num_relocs, static_cast<BinarySection>(section),
           section_name);
  for (Index i = 0; i < num_relocs; ++i) {
    Offset offset;
    Index index;
    uint32_t reloc_type, addend = 0;
    CHECK_RESULT(ReadU32Leb128(&reloc_type, "relocation type"));
    CHECK_RESULT(ReadOffset(&offset, "offset"));
    CHECK_RESULT(ReadIndex(&index, "index"));
    RelocType type = static_cast<RelocType>(reloc_type);
    switch (type) {
      case RelocType::GlobalAddressLEB:
      case RelocType::GlobalAddressSLEB:
      case RelocType::GlobalAddressI32:
        CHECK_RESULT(ReadI32Leb128(&addend, "addend"));
        break;
      default:
        break;
    }
    CALLBACK(OnReloc, type, offset, index, addend);
  }
  CALLBACK0(EndRelocSection);
  return Result::Ok;
}

Result BinaryReader::ReadLinkingSection(Offset section_size) {
  CALLBACK(BeginLinkingSection, section_size);
  Offset previous_read_end = read_end_;
  while (state_.offset < read_end_) {
    uint32_t linking_type;
    Offset subsection_size;
    CHECK_RESULT(ReadU32Leb128(&linking_type, "type"));
    CHECK_RESULT(ReadOffset(&subsection_size, "subsection size"));
    size_t subsection_end = state_.offset + subsection_size;
    ERROR_UNLESS(subsection_end <= read_end_,
                 "invalid sub-section size: extends past end");
    read_end_ = subsection_end;

    switch (static_cast<LinkingEntryType>(linking_type)) {
      case LinkingEntryType::StackPointer: {
        uint32_t stack_ptr;
        CHECK_RESULT(ReadU32Leb128(&stack_ptr, "stack pointer index"));
        CALLBACK(OnStackGlobal, stack_ptr);
        break;
      }
      case LinkingEntryType::SymbolInfo: {
        uint32_t info_count;
        CHECK_RESULT(ReadU32Leb128(&info_count, "info count"));
        CALLBACK(OnSymbolInfoCount, info_count);
        while (info_count--) {
          string_view name;
          uint32_t info;
          CHECK_RESULT(ReadStr(&name, "symbol name"));
          CHECK_RESULT(ReadU32Leb128(&info, "sym flags"));
          CALLBACK(OnSymbolInfo, name, info);
        }
        break;
      }
      default:
        /* unknown subsection, skip it */
        state_.offset = subsection_end;
        break;
    }
    ERROR_UNLESS(state_.offset == subsection_end,
                 "unfinished sub-section (expected end: 0x%" PRIzx ")",
                 subsection_end);
    read_end_ = previous_read_end;
  }
  CALLBACK0(EndLinkingSection);
  return Result::Ok;
}

Result BinaryReader::ReadExceptionType(TypeVector& sig) {
  Index num_values;
  CHECK_RESULT(ReadIndex(&num_values, "exception type count"));
  sig.resize(num_values);
  for (Index j = 0; j < num_values; ++j) {
    Type value_type;
    CHECK_RESULT(ReadType(&value_type, "exception value type"));
    ERROR_UNLESS(is_concrete_type(value_type),
                 "excepted valid exception value type (got %d)",
                 static_cast<int>(value_type));
    sig[j] = value_type;
  }
  return Result::Ok;
}

Result BinaryReader::ReadExceptionSection(Offset section_size) {
  CALLBACK(BeginExceptionSection, section_size);
  CHECK_RESULT(ReadIndex(&num_exceptions_, "exception count"));
  CALLBACK(OnExceptionCount, num_exceptions_);

  for (Index i = 0; i < num_exceptions_; ++i) {
    TypeVector sig;
    CHECK_RESULT(ReadExceptionType(sig));
    CALLBACK(OnExceptionType, i, sig);
  }

  CALLBACK(EndExceptionSection);
  return Result::Ok;
}

Result BinaryReader::ReadCustomSection(Offset section_size) {
  string_view section_name;
  CHECK_RESULT(ReadStr(&section_name, "section name"));
  CALLBACK(BeginCustomSection, section_size, section_name);

  bool name_section_ok = last_known_section_ >= BinarySection::Import;
  if (options_->read_debug_names && name_section_ok &&
      section_name == WABT_BINARY_SECTION_NAME) {
    CHECK_RESULT(ReadNamesSection(section_size));
  } else if (section_name.rfind(WABT_BINARY_SECTION_RELOC, 0) == 0) {
    // Reloc sections always begin with "reloc."
    CHECK_RESULT(ReadRelocSection(section_size));
  } else if (section_name == WABT_BINARY_SECTION_LINKING) {
    CHECK_RESULT(ReadLinkingSection(section_size));
  } else if (options_->features.exceptions_enabled() &&
             section_name == WABT_BINARY_SECTION_EXCEPTION) {
    CHECK_RESULT(ReadExceptionSection(section_size));
  } else {
    /* This is an unknown custom section, skip it. */
    state_.offset = read_end_;
  }
  CALLBACK0(EndCustomSection);
  return Result::Ok;
}

Result BinaryReader::ReadTypeSection(Offset section_size) {
  CALLBACK(BeginTypeSection, section_size);
  CHECK_RESULT(ReadIndex(&num_signatures_, "type count"));
  CALLBACK(OnTypeCount, num_signatures_);

  for (Index i = 0; i < num_signatures_; ++i) {
    Type form;
    CHECK_RESULT(ReadType(&form, "type form"));
    ERROR_UNLESS(form == Type::Func, "unexpected type form: %d",
                 static_cast<int>(form));

    Index num_params;
    CHECK_RESULT(ReadIndex(&num_params, "function param count"));

    param_types_.resize(num_params);

    for (Index j = 0; j < num_params; ++j) {
      Type param_type;
      CHECK_RESULT(ReadType(&param_type, "function param type"));
      ERROR_UNLESS(is_concrete_type(param_type),
                   "expected valid param type (got %d)",
                   static_cast<int>(param_type));
      param_types_[j] = param_type;
    }

    Index num_results;
    CHECK_RESULT(ReadIndex(&num_results, "function result count"));
    ERROR_UNLESS(num_results <= 1, "result count must be 0 or 1");

    Type result_type = Type::Void;
    if (num_results) {
      CHECK_RESULT(ReadType(&result_type, "function result type"));
      ERROR_UNLESS(is_concrete_type(result_type),
                   "expected valid result type: %d",
                   static_cast<int>(result_type));
    }

    Type* param_types = num_params ? param_types_.data() : nullptr;

    CALLBACK(OnType, i, num_params, param_types, num_results, &result_type);
  }
  CALLBACK0(EndTypeSection);
  return Result::Ok;
}

Result BinaryReader::ReadImportSection(Offset section_size) {
  CALLBACK(BeginImportSection, section_size);
  CHECK_RESULT(ReadIndex(&num_imports_, "import count"));
  CALLBACK(OnImportCount, num_imports_);
  for (Index i = 0; i < num_imports_; ++i) {
    string_view module_name;
    CHECK_RESULT(ReadStr(&module_name, "import module name"));
    string_view field_name;
    CHECK_RESULT(ReadStr(&field_name, "import field name"));

    uint32_t kind;
    CHECK_RESULT(ReadU32Leb128(&kind, "import kind"));
    switch (static_cast<ExternalKind>(kind)) {
      case ExternalKind::Func: {
        Index sig_index;
        CHECK_RESULT(ReadIndex(&sig_index, "import signature index"));
        ERROR_UNLESS(sig_index < num_signatures_,
                     "invalid import signature index");
        CALLBACK(OnImport, i, module_name, field_name);
        CALLBACK(OnImportFunc, i, module_name, field_name, num_func_imports_,
                 sig_index);
        num_func_imports_++;
        break;
      }

      case ExternalKind::Table: {
        Type elem_type;
        Limits elem_limits;
        CHECK_RESULT(ReadTable(&elem_type, &elem_limits));
        CALLBACK(OnImport, i, module_name, field_name);
        CALLBACK(OnImportTable, i, module_name, field_name, num_table_imports_,
                 elem_type, &elem_limits);
        num_table_imports_++;
        break;
      }

      case ExternalKind::Memory: {
        Limits page_limits;
        CHECK_RESULT(ReadMemory(&page_limits));
        CALLBACK(OnImport, i, module_name, field_name);
        CALLBACK(OnImportMemory, i, module_name, field_name,
                 num_memory_imports_, &page_limits);
        num_memory_imports_++;
        break;
      }

      case ExternalKind::Global: {
        Type type;
        bool mutable_;
        CHECK_RESULT(ReadGlobalHeader(&type, &mutable_));
        CALLBACK(OnImport, i, module_name, field_name);
        CALLBACK(OnImportGlobal, i, module_name, field_name,
                 num_global_imports_, type, mutable_);
        num_global_imports_++;
        break;
      }

      case ExternalKind::Except: {
        ERROR_UNLESS(options_->features.exceptions_enabled(),
                     "invalid import exception kind: exceptions not allowed");
        TypeVector sig;
        CHECK_RESULT(ReadExceptionType(sig));
        CALLBACK(OnImport, i, module_name, field_name);
        CALLBACK(OnImportException, i, module_name, field_name,
                 num_exception_imports_, sig);
        num_exception_imports_++;
        break;
      }

      default:
        PrintError("invalid import kind: %d", kind);
        return Result::Error;
    }
  }
  CALLBACK0(EndImportSection);
  return Result::Ok;
}

Result BinaryReader::ReadFunctionSection(Offset section_size) {
  CALLBACK(BeginFunctionSection, section_size);
  CHECK_RESULT(
      ReadIndex(&num_function_signatures_, "function signature count"));
  CALLBACK(OnFunctionCount, num_function_signatures_);
  for (Index i = 0; i < num_function_signatures_; ++i) {
    Index func_index = num_func_imports_ + i;
    Index sig_index;
    CHECK_RESULT(ReadIndex(&sig_index, "function signature index"));
    ERROR_UNLESS(sig_index < num_signatures_,
                 "invalid function signature index: %" PRIindex, sig_index);
    CALLBACK(OnFunction, func_index, sig_index);
  }
  CALLBACK0(EndFunctionSection);
  return Result::Ok;
}

Result BinaryReader::ReadTableSection(Offset section_size) {
  CALLBACK(BeginTableSection, section_size);
  CHECK_RESULT(ReadIndex(&num_tables_, "table count"));
  ERROR_UNLESS(num_tables_ <= 1, "table count (%" PRIindex ") must be 0 or 1",
               num_tables_);
  CALLBACK(OnTableCount, num_tables_);
  for (Index i = 0; i < num_tables_; ++i) {
    Index table_index = num_table_imports_ + i;
    Type elem_type;
    Limits elem_limits;
    CHECK_RESULT(ReadTable(&elem_type, &elem_limits));
    CALLBACK(OnTable, table_index, elem_type, &elem_limits);
  }
  CALLBACK0(EndTableSection);
  return Result::Ok;
}

Result BinaryReader::ReadMemorySection(Offset section_size) {
  CALLBACK(BeginMemorySection, section_size);
  CHECK_RESULT(ReadIndex(&num_memories_, "memory count"));
  ERROR_UNLESS(num_memories_ <= 1, "memory count must be 0 or 1");
  CALLBACK(OnMemoryCount, num_memories_);
  for (Index i = 0; i < num_memories_; ++i) {
    Index memory_index = num_memory_imports_ + i;
    Limits page_limits;
    CHECK_RESULT(ReadMemory(&page_limits));
    CALLBACK(OnMemory, memory_index, &page_limits);
  }
  CALLBACK0(EndMemorySection);
  return Result::Ok;
}

Result BinaryReader::ReadGlobalSection(Offset section_size) {
  CALLBACK(BeginGlobalSection, section_size);
  CHECK_RESULT(ReadIndex(&num_globals_, "global count"));
  CALLBACK(OnGlobalCount, num_globals_);
  for (Index i = 0; i < num_globals_; ++i) {
    Index global_index = num_global_imports_ + i;
    Type global_type;
    bool mutable_;
    CHECK_RESULT(ReadGlobalHeader(&global_type, &mutable_));
    CALLBACK(BeginGlobal, global_index, global_type, mutable_);
    CALLBACK(BeginGlobalInitExpr, global_index);
    CHECK_RESULT(ReadInitExpr(global_index));
    CALLBACK(EndGlobalInitExpr, global_index);
    CALLBACK(EndGlobal, global_index);
  }
  CALLBACK0(EndGlobalSection);
  return Result::Ok;
}

Result BinaryReader::ReadExportSection(Offset section_size) {
  CALLBACK(BeginExportSection, section_size);
  CHECK_RESULT(ReadIndex(&num_exports_, "export count"));
  CALLBACK(OnExportCount, num_exports_);
  for (Index i = 0; i < num_exports_; ++i) {
    string_view name;
    CHECK_RESULT(ReadStr(&name, "export item name"));

    uint8_t external_kind = 0;
    CHECK_RESULT(ReadU8(&external_kind, "export external kind"));
    ERROR_UNLESS(is_valid_external_kind(external_kind),
                 "invalid export external kind: %d", external_kind);

    Index item_index;
    CHECK_RESULT(ReadIndex(&item_index, "export item index"));
    switch (static_cast<ExternalKind>(external_kind)) {
      case ExternalKind::Func:
        ERROR_UNLESS(item_index < NumTotalFuncs(),
                     "invalid export func index: %" PRIindex, item_index);
        break;
      case ExternalKind::Table:
        ERROR_UNLESS(item_index < NumTotalTables(),
                     "invalid export table index: %" PRIindex, item_index);
        break;
      case ExternalKind::Memory:
        ERROR_UNLESS(item_index < NumTotalMemories(),
                     "invalid export memory index: %" PRIindex, item_index);
        break;
      case ExternalKind::Global:
        ERROR_UNLESS(item_index < NumTotalGlobals(),
                     "invalid export global index: %" PRIindex, item_index);
        break;
      case ExternalKind::Except:
        // Note: Can't check if index valid, exceptions section comes later.
        ERROR_UNLESS(options_->features.exceptions_enabled(),
                     "invalid export exception kind: exceptions not allowed");
        break;
    }

    CALLBACK(OnExport, i, static_cast<ExternalKind>(external_kind), item_index,
             name);
  }
  CALLBACK0(EndExportSection);
  return Result::Ok;
}

Result BinaryReader::ReadStartSection(Offset section_size) {
  CALLBACK(BeginStartSection, section_size);
  Index func_index;
  CHECK_RESULT(ReadIndex(&func_index, "start function index"));
  ERROR_UNLESS(func_index < NumTotalFuncs(),
               "invalid start function index: %" PRIindex, func_index);
  CALLBACK(OnStartFunction, func_index);
  CALLBACK0(EndStartSection);
  return Result::Ok;
}

Result BinaryReader::ReadElemSection(Offset section_size) {
  CALLBACK(BeginElemSection, section_size);
  Index num_elem_segments;
  CHECK_RESULT(ReadIndex(&num_elem_segments, "elem segment count"));
  CALLBACK(OnElemSegmentCount, num_elem_segments);
  ERROR_UNLESS(num_elem_segments == 0 || NumTotalTables() > 0,
               "elem section without table section");
  for (Index i = 0; i < num_elem_segments; ++i) {
    Index table_index;
    CHECK_RESULT(ReadIndex(&table_index, "elem segment table index"));
    CALLBACK(BeginElemSegment, i, table_index);
    CALLBACK(BeginElemSegmentInitExpr, i);
    CHECK_RESULT(ReadInitExpr(i));
    CALLBACK(EndElemSegmentInitExpr, i);

    Index num_function_indexes;
    CHECK_RESULT(
        ReadIndex(&num_function_indexes, "elem segment function index count"));
    CALLBACK(OnElemSegmentFunctionIndexCount, i, num_function_indexes);
    for (Index j = 0; j < num_function_indexes; ++j) {
      Index func_index;
      CHECK_RESULT(ReadIndex(&func_index, "elem segment function index"));
      CALLBACK(OnElemSegmentFunctionIndex, i, func_index);
    }
    CALLBACK(EndElemSegment, i);
  }
  CALLBACK0(EndElemSection);
  return Result::Ok;
}

Result BinaryReader::ReadCodeSection(Offset section_size) {
  CALLBACK(BeginCodeSection, section_size);
  CHECK_RESULT(ReadIndex(&num_function_bodies_, "function body count"));
  ERROR_UNLESS(num_function_signatures_ == num_function_bodies_,
               "function signature count != function body count");
  CALLBACK(OnFunctionBodyCount, num_function_bodies_);
  for (Index i = 0; i < num_function_bodies_; ++i) {
    Index func_index = num_func_imports_ + i;
    Offset func_offset = state_.offset;
    state_.offset = func_offset;
    CALLBACK(BeginFunctionBody, func_index);
    uint32_t body_size;
    CHECK_RESULT(ReadU32Leb128(&body_size, "function body size"));
    Offset body_start_offset = state_.offset;
    Offset end_offset = body_start_offset + body_size;

    Index num_local_decls;
    CHECK_RESULT(ReadIndex(&num_local_decls, "local declaration count"));
    CALLBACK(OnLocalDeclCount, num_local_decls);
    for (Index k = 0; k < num_local_decls; ++k) {
      Index num_local_types;
      CHECK_RESULT(ReadIndex(&num_local_types, "local type count"));
      Type local_type;
      CHECK_RESULT(ReadType(&local_type, "local type"));
      ERROR_UNLESS(is_concrete_type(local_type), "expected valid local type");
      CALLBACK(OnLocalDecl, k, num_local_types, local_type);
    }

    CHECK_RESULT(ReadFunctionBody(end_offset));

    CALLBACK(EndFunctionBody, func_index);
  }
  CALLBACK0(EndCodeSection);
  return Result::Ok;
}

Result BinaryReader::ReadDataSection(Offset section_size) {
  CALLBACK(BeginDataSection, section_size);
  Index num_data_segments;
  CHECK_RESULT(ReadIndex(&num_data_segments, "data segment count"));
  CALLBACK(OnDataSegmentCount, num_data_segments);
  ERROR_UNLESS(num_data_segments == 0 || NumTotalMemories() > 0,
               "data section without memory section");
  for (Index i = 0; i < num_data_segments; ++i) {
    Index memory_index;
    CHECK_RESULT(ReadIndex(&memory_index, "data segment memory index"));
    CALLBACK(BeginDataSegment, i, memory_index);
    CALLBACK(BeginDataSegmentInitExpr, i);
    CHECK_RESULT(ReadInitExpr(i));
    CALLBACK(EndDataSegmentInitExpr, i);

    Address data_size;
    const void* data;
    CHECK_RESULT(ReadBytes(&data, &data_size, "data segment data"));
    CALLBACK(OnDataSegmentData, i, data, data_size);
    CALLBACK(EndDataSegment, i);
  }
  CALLBACK0(EndDataSection);
  return Result::Ok;
}

Result BinaryReader::ReadSections() {
  while (state_.offset < state_.size) {
    uint32_t section_code;
    Offset section_size;
    /* Temporarily reset read_end_ to the full data size so the next section
     * can be read. */
    read_end_ = state_.size;
    CHECK_RESULT(ReadU32Leb128(&section_code, "section code"));
    CHECK_RESULT(ReadOffset(&section_size, "section size"));
    read_end_ = state_.offset + section_size;
    if (section_code >= kBinarySectionCount) {
      PrintError("invalid section code: %u; max is %u", section_code,
                 kBinarySectionCount - 1);
      return Result::Error;
    }

    BinarySection section = static_cast<BinarySection>(section_code);

    ERROR_UNLESS(read_end_ <= state_.size,
                 "invalid section size: extends past end");

    ERROR_UNLESS(last_known_section_ == BinarySection::Invalid ||
                     section == BinarySection::Custom ||
                     section > last_known_section_,
                 "section %s out of order", GetSectionName(section));

    CALLBACK(BeginSection, section, section_size);

#define V(Name, name, code)                          \
  case BinarySection::Name:                          \
    CHECK_RESULT(Read##Name##Section(section_size)); \
    break;

    switch (section) {
      WABT_FOREACH_BINARY_SECTION(V)

      default:
        assert(0);
        break;
    }

#undef V

    ERROR_UNLESS(state_.offset == read_end_,
                 "unfinished section (expected end: 0x%" PRIzx ")", read_end_);

    if (section != BinarySection::Custom)
      last_known_section_ = section;
  }
  return Result::Ok;
}

Result BinaryReader::ReadModule() {
  uint32_t magic = 0;
  CHECK_RESULT(ReadU32(&magic, "magic"));
  ERROR_UNLESS(magic == WABT_BINARY_MAGIC, "bad magic value");
  uint32_t version = 0;
  CHECK_RESULT(ReadU32(&version, "version"));
  ERROR_UNLESS(version == WABT_BINARY_VERSION,
               "bad wasm file version: %#x (expected %#x)", version,
               WABT_BINARY_VERSION);

  CALLBACK(BeginModule, version);
  CHECK_RESULT(ReadSections());
  CALLBACK0(EndModule);

  return Result::Ok;
}

}  // end anonymous namespace

Result ReadBinary(const void* data,
                  size_t size,
                  BinaryReaderDelegate* delegate,
                  const ReadBinaryOptions* options) {
  BinaryReader reader(data, size, delegate, options);
  return reader.ReadModule();
}

}  // namespace wabt
