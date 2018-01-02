/*
 * Copyright 2017 wasmjit-omr project participants
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

#include "function-builder.h"
#include "src/interp.h"
#include "ilgen/VirtualMachineState.hpp"
#include "infra/Assert.hpp"
#include <type_traits>

namespace wabt {
namespace jit {

// The following functions are required to be able to properly parse opcodes. However, their
// original definitions are defined with static linkage in src/interp.cc. Because of this, the only
// way to use them is to simply copy their definitions here.

template <typename T>
inline T ReadUxAt(const uint8_t* pc) {
  T result;
  memcpy(&result, pc, sizeof(T));
  return result;
}

template <typename T>
inline T ReadUx(const uint8_t** pc) {
  T result = ReadUxAt<T>(*pc);
  *pc += sizeof(T);
  return result;
}

inline uint8_t ReadU8(const uint8_t** pc) {
  return ReadUx<uint8_t>(pc);
}

inline uint32_t ReadU32(const uint8_t** pc) {
  return ReadUx<uint32_t>(pc);
}

inline uint64_t ReadU64(const uint8_t** pc) {
  return ReadUx<uint64_t>(pc);
}

inline Opcode ReadOpcode(const uint8_t** pc) {
  uint8_t value = ReadU8(pc);
  if (Opcode::IsPrefixByte(value)) {
    // For now, assume all instructions are encoded with just one extra byte
    // so we don't have to decode LEB128 here.
    uint32_t code = ReadU8(pc);
    return Opcode::FromCode(value, code);
  } else {
    // TODO(binji): Optimize if needed; Opcode::FromCode does a log2(n) lookup
    // from the encoding.
    return Opcode::FromCode(value);
  }
}

inline Opcode ReadOpcodeAt(const uint8_t* pc) {
  return ReadOpcode(&pc);
}

FunctionBuilder::FunctionBuilder(interp::Thread* thread, interp::IstreamOffset const offset, TypeDictionary* types)
    : TR::MethodBuilder(types),
      thread_(thread),
      offset_(offset),
      valueType_(types->LookupUnion("Value")),
      pValueType_(types->PointerTo(types->LookupUnion("Value"))) {
  DefineLine(__LINE__);
  DefineFile(__FILE__);
  DefineName("WASM_Function");

  DefineReturnType(types->toIlType<std::underlying_type<wabt::interp::Result>::type>());
}

bool FunctionBuilder::buildIL() {
  setVMState(new OMR::VirtualMachineState());

  const uint8_t* istream = thread_->GetIstream();

  workItems_.emplace_back(OrphanBytecodeBuilder(0, const_cast<char*>(ReadOpcodeAt(&istream[offset_]).GetName())),
                          &istream[offset_]);
  AppendBuilder(workItems_[0].builder);

  int32_t next_index;

  while ((next_index = GetNextBytecodeFromWorklist()) != -1) {
    auto& work_item = workItems_[next_index];

    if (!Emit(work_item.builder, istream, work_item.pc))
      return false;
  }

  return true;
}

/**
 * @brief Generate push to the interpreter stack
 *
 * The generated code should be equivalent to:
 *
 * auto stack_top = *stack_top_addr;
 * stack_base_addr[stack_top] = value;
 * *stack_top_addr = stack_top + 1;
 */
void FunctionBuilder::Push(TR::IlBuilder* b, const char* type, TR::IlValue* value) {
  using ResultEnum = std::underlying_type<wabt::interp::Result>::type;

  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
  auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

  auto* stack_top = b->LoadAt(pInt32, stack_top_addr);

  TR::IlBuilder* overflow_handler = nullptr;

  b->IfThen(&overflow_handler,
  b->       UnsignedGreaterOrEqualTo(
                stack_top,
  b->           Const(static_cast<int32_t>(thread_->value_stack_.size()))));
  overflow_handler->Return(
  overflow_handler->    Const(static_cast<ResultEnum>(interp::Result::TrapValueStackExhausted)));

  b->StoreIndirect("Value", type,
  b->              IndexAt(pValueType_,
                           stack_base_addr,
                           stack_top),
                   value);
  b->StoreAt(stack_top_addr,
  b->        Add(
                 stack_top,
  b->            Const(1)));
}

/**
 * @brief Generate pop from the interpreter stack
 *
 * The generated code should be equivalent to:
 *
 * auto new_stack_top = *stack_top_addr - 1;
 * *stack_top_addr = new_stack_top;
 * return stack_base_addr[new_stack_top];
 */
TR::IlValue* FunctionBuilder::Pop(TR::IlBuilder* b, const char* type) {
  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
  auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

  auto* new_stack_top = b->Sub(
                        b->    LoadAt(pInt32, stack_top_addr),
                        b->    Const(1));
  b->StoreAt(stack_top_addr, new_stack_top);
  return b->LoadIndirect("Value", type,
         b->             IndexAt(pValueType_,
                                 stack_base_addr,
                                 new_stack_top));
}

/**
 * @brief Generate a drop-x from the interpreter stack, optionally keeping the top value
 *
 * The generated code should be equivalent to:
 *
 * auto stack_top = *stack_top_addr;
 * auto new_stack_top = stack_top - drop_count;
 *
 * if (keep_count == 1) {
 *   stack_base_addr[new_stack_top - 1] = stack_base_addr[stack_top - 1];
 * }
 *
 * *stack_top_addr = new_stack_top;
 */
void FunctionBuilder::DropKeep(TR::IlBuilder* b, uint32_t drop_count, uint8_t keep_count) {
  TR_ASSERT(keep_count <= 1, "Invalid keep count");

  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
  auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

  auto* stack_top = b->LoadAt(pInt32, stack_top_addr);
  auto* new_stack_top = b->Sub(stack_top, b->Const(static_cast<int32_t>(drop_count)));

  if (keep_count == 1) {
    auto* old_top_value = b->LoadAt(pValueType_,
                          b->       IndexAt(pValueType_,
                                            stack_base_addr,
                          b->               Sub(stack_top, b->Const(1))));

    b->StoreAt(
    b->        IndexAt(pValueType_,
                       stack_base_addr,
    b->                Sub(new_stack_top, b->Const(1))),
               old_top_value);
  }

  b->StoreAt(stack_top_addr, new_stack_top);
}

bool FunctionBuilder::Emit(TR::BytecodeBuilder* b,
                           const uint8_t* istream,
                           const uint8_t* pc) {
  using ValueEnum = std::underlying_type<wabt::interp::Result>::type;

  Opcode opcode = ReadOpcode(&pc);
  TR_ASSERT(!opcode.IsInvalid(), "Invalid opcode");

  switch (opcode) {
    case Opcode::Select: {
      TR::IlBuilder* true_path = nullptr;
      TR::IlBuilder* false_path = nullptr;

      b->IfThenElse(&true_path, &false_path, Pop(b, "i32"));
      DropKeep(true_path, 1, 0);
      DropKeep(false_path, 1, 1);
      break;
    }

    case Opcode::Return:
      b->Return(b->Const(static_cast<ValueEnum>(interp::Result::Ok)));
      return true;

    case Opcode::Unreachable:
      b->Return(b->Const(static_cast<ValueEnum>(interp::Result::TrapUnreachable)));
      return true;

    case Opcode::I32Const:
      Push(b, "i32", b->ConstInt32(ReadU32(&pc)));
      break;

    case Opcode::I64Const:
      Push(b, "i64", b->ConstInt64(ReadU64(&pc)));
      break;

    case Opcode::F32Const:
      Push(b, "f32", b->ConstInt32(ReadU32(&pc)));
      break;

    case Opcode::F64Const:
      Push(b, "f64", b->ConstInt64(ReadU64(&pc)));
      break;

    case Opcode::Drop:
      DropKeep(b, 1, 0);
      break;

    case Opcode::Nop:
      break;

    default:
      return false;
  }

  int32_t next_index = static_cast<int32_t>(workItems_.size());

  workItems_.emplace_back(OrphanBytecodeBuilder(next_index,
                                                const_cast<char*>(ReadOpcodeAt(pc).GetName())),
                          pc);
  b->AddFallThroughBuilder(workItems_[next_index].builder);
  return true;
}

}
}