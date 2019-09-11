#include "LogManager.h"
#include "Common/MathUtils.h"
#include "Interface/Context/Context.h"
#include "Interface/Core/DebugData.h"
#include "Interface/Core/InternalThreadState.h"
#include "Interface/HLE/Syscalls.h"
#include "LogManager.h"

#include <FEXCore/Core/CPUBackend.h>
#include <FEXCore/IR/IR.h>
#include <FEXCore/IR/IntrusiveIRList.h>

#include <atomic>
#include <vector>

namespace FEXCore::CPU {

#define DESTMAP_AS_MAP 0
#if DESTMAP_AS_MAP
using DestMapType = std::unordered_map<uint64_t, void*>;
#else
using DestMapType = std::vector<void*>;
#endif

class InterpreterCore final : public CPUBackend {
public:
  explicit InterpreterCore(FEXCore::Context::Context *ctx);
  ~InterpreterCore() override = default;
  std::string GetName() override { return "Interpreter"; }
  void *CompileCode(FEXCore::IR::IRListView<true> const *IR, FEXCore::Core::DebugData *DebugData) override;

  void *MapRegion(void* HostPtr, uint64_t, uint64_t) override { return HostPtr; }

  bool NeedsOpDispatch() override { return true; }

  void ExecuteCode(FEXCore::Core::InternalThreadState *Thread);
private:
  FEXCore::Context::Context *CTX;
  void *AllocateTmpSpace(size_t Size);

  template<typename Res>
  Res GetDest(IR::NodeWrapper Op);

  template<typename Res>
  Res GetSrc(IR::NodeWrapper Src);

  std::vector<uint8_t> TmpSpace;
  DestMapType DestMap;
  size_t TmpOffset{};

  FEXCore::IR::IRListView<true> *CurrentIR;
};

static void InterpreterExecution(FEXCore::Core::InternalThreadState *Thread) {
  InterpreterCore *Core = reinterpret_cast<InterpreterCore*>(Thread->CPUBackend.get());
  Core->ExecuteCode(Thread);
}

InterpreterCore::InterpreterCore(FEXCore::Context::Context *ctx)
  : CTX {ctx} {
  // Grab our space for temporary data
  TmpSpace.resize(4096 * 32);
#if !DESTMAP_AS_MAP
  DestMap.resize(4096);
#endif
}

void *InterpreterCore::AllocateTmpSpace(size_t Size) {
  // XXX: IR generation has a bug where the size can periodically end up being zero
  // LogMan::Throw::A(Size !=0, "Dest Op had zero destination size");
  Size = Size < 16 ? 16 : Size;

  // Force alignment by size
  size_t NewBase = AlignUp(TmpOffset, Size);
  size_t NewEnd = NewBase + Size;

  if (NewEnd >= TmpSpace.size()) {
    // If we are going to overrun the end of our temporary space then double the size of it
    TmpSpace.resize(TmpSpace.size() * 2);
  }

  // Make sure to set the new offset
  TmpOffset = NewEnd;

  return &TmpSpace.at(NewBase);
}

template<typename Res>
Res InterpreterCore::GetDest(IR::NodeWrapper Op) {
  auto DstPtr = DestMap[Op.NodeOffset];
  return reinterpret_cast<Res>(DstPtr);
}

template<typename Res>
Res InterpreterCore::GetSrc(IR::NodeWrapper Src) {
#if DESTMAP_AS_MAP
  LogMan::Throw::A(DestMap.find(Src.NodeOffset) != DestMap.end(), "Op had source but it wasn't in the destination map");
#endif

  auto DstPtr = DestMap[Src.NodeOffset];
  LogMan::Throw::A(DstPtr != nullptr, "Destmap had slot but didn't get allocated memory");
  return reinterpret_cast<Res>(DstPtr);
}

void *InterpreterCore::CompileCode([[maybe_unused]] FEXCore::IR::IRListView<true> const *IR, [[maybe_unused]] FEXCore::Core::DebugData *DebugData) {
  return reinterpret_cast<void*>(InterpreterExecution);
}

void InterpreterCore::ExecuteCode(FEXCore::Core::InternalThreadState *Thread) {
   auto IR = Thread->IRLists.find(Thread->State.State.rip);
   auto DebugData = Thread->DebugData.find(Thread->State.State.rip);
   CurrentIR = IR->second.get();

   bool Quit = false;
   TmpOffset = 0; // Reset where we are in the temp data range

   uintptr_t ListBegin = CurrentIR->GetListData();
   uintptr_t DataBegin = CurrentIR->GetData();

   IR::NodeWrapperIterator Begin = CurrentIR->begin();
   IR::NodeWrapperIterator End = CurrentIR->end();

 #if DESTMAP_AS_MAP
   DestMap.clear();
 #else
   uintptr_t ListSize = CurrentIR->GetListSize();
   if (ListSize > DestMap.size()) {
     DestMap.resize(std::max(DestMap.size() * 2, ListSize));
   }
 #endif

 static_assert(sizeof(FEXCore::IR::IROp_Header) == 4);
 static_assert(sizeof(FEXCore::IR::OrderedNode) == 16);

 #define GD *GetDest<uint64_t*>(*WrapperOp)
 #define GDP GetDest<void*>(*WrapperOp)
   while (Begin != End && !Quit) {
     using namespace FEXCore::IR;
     using namespace FEXCore::IR;

     NodeWrapper *WrapperOp = Begin();
     OrderedNode *RealNode = reinterpret_cast<OrderedNode*>(WrapperOp->GetPtr(ListBegin));
     FEXCore::IR::IROp_Header *IROp = RealNode->Op(DataBegin);
     uint8_t OpSize = IROp->Size;

     if (IROp->HasDest) {
       uint64_t AllocSize = OpSize * std::min(static_cast<uint8_t>(1), IROp->Elements);
       DestMap[WrapperOp->NodeOffset] = AllocateTmpSpace(AllocSize);
     }

     switch (IROp->Op) {
     case IR::OP_BEGINBLOCK:
     break;
     case IR::OP_ENDBLOCK: {
       auto Op = IROp->C<IR::IROp_EndBlock>();
       Thread->State.State.rip += Op->RIPIncrement;
     break;
     }
    case IR::OP_EXITFUNCTION:
    case IR::OP_ENDFUNCTION: {
      Quit = true;
    break;
    }

     case IR::OP_MOV: {
       auto Op = IROp->C<IR::IROp_Mov>();
       memcpy(GDP, GetSrc<void*>(Op->Header.Args[0]), OpSize);
     break;
     }
     case IR::OP_BREAK: {
       auto Op = IROp->C<IR::IROp_Break>();
       switch (Op->Reason) {
       case 4: // HLT
         Thread->State.RunningEvents.ShouldStop = true;
         Quit = true;
       break;
       default: LogMan::Msg::A("Unknown Break reason: %d", Op->Reason);
       }
     }
     break;
    case IR::OP_CONDJUMP: {
      auto Op = IROp->C<IR::IROp_CondJump>();
      uint64_t Arg = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      if (!!Arg) {
        // Convert argument from NodeWrapper to NodeWrapperIterator
        auto IterLocation = NodeWrapperIterator(ListBegin, Op->Header.Args[1]);
        Begin = IterLocation;
        continue;
      }
    break;
    }
    case IR::OP_JUMP: {
      auto Op = IROp->C<IR::IROp_Jump>();
      // Convert argument from NodeWrapper to NodeWrapperIterator
      auto IterLocation = NodeWrapperIterator(ListBegin, Op->Header.Args[0]);
      Begin = IterLocation;
      continue;
    break;
    }
    case IR::OP_CONSTANT: {
      auto Op = IROp->C<IR::IROp_Constant>();
      GD = Op->Constant;
    break;
    }
    case IR::OP_LOADCONTEXT: {
      auto Op = IROp->C<IR::IROp_LoadContext>();

      uintptr_t ContextPtr = reinterpret_cast<uintptr_t>(&Thread->State.State);
      ContextPtr += Op->Offset;
#define LOAD_CTX(x, y) \
    case x: { \
      y const *Data = reinterpret_cast<y const*>(ContextPtr); \
      GD = *Data; \
    } \
    break
      switch (Op->Size) {
      LOAD_CTX(1, uint8_t);
      LOAD_CTX(2, uint16_t);
      LOAD_CTX(4, uint32_t);
      LOAD_CTX(8, uint64_t);
      case 16: {
        void const *Data = reinterpret_cast<void const*>(ContextPtr);
        memcpy(GDP, Data, Op->Size);
      }
      break;
      default:  LogMan::Msg::A("Unhandled LoadContext size: %d", Op->Size);
      }
#undef LOAD_CTX
    break;
    }
    case IR::OP_LOADFLAG: {
      auto Op = IROp->C<IR::IROp_LoadFlag>();

      uintptr_t ContextPtr = reinterpret_cast<uintptr_t>(&Thread->State.State);
      ContextPtr += offsetof(FEXCore::Core::CPUState, flags[0]);
      ContextPtr += Op->Flag;
      uint8_t const *Data = reinterpret_cast<uint8_t const*>(ContextPtr);
      GD = *Data;
    break;
    }
    case IR::OP_STOREFLAG: {
      auto Op = IROp->C<IR::IROp_StoreFlag>();
      uint8_t Arg = *GetSrc<uint8_t*>(Op->Header.Args[0]) & 1;

      uintptr_t ContextPtr = reinterpret_cast<uintptr_t>(&Thread->State.State);
      ContextPtr += offsetof(FEXCore::Core::CPUState, flags[0]);
      ContextPtr += Op->Flag;
      uint8_t *Data = reinterpret_cast<uint8_t*>(ContextPtr);
      *Data = Arg;
    break;
    }

     case IR::OP_STORECONTEXT: {
       auto Op = IROp->C<IR::IROp_StoreContext>();

       uintptr_t ContextPtr = reinterpret_cast<uintptr_t>(&Thread->State.State);
       ContextPtr += Op->Offset;

       void *Data = reinterpret_cast<void*>(ContextPtr);
       void *Src = GetSrc<void*>(Op->Header.Args[0]);
       memcpy(Data, Src, Op->Size);
     break;
     }
     case IR::OP_SYSCALL: {
       auto Op = IROp->C<IR::IROp_Syscall>();

       FEXCore::HLE::SyscallArguments Args;
       for (size_t j = 0; j < 7; ++j)
         Args.Argument[j] = *GetSrc<uint64_t*>(Op->Header.Args[j]);

       uint64_t Res = CTX->SyscallHandler.HandleSyscall(Thread, &Args);
       GD = Res;
     break;
     }
     case IR::OP_LOADMEM: {
       auto Op = IROp->C<IR::IROp_LoadMem>();
       void const *Data = Thread->CTX->MemoryMapper.GetPointer<void const*>(*GetSrc<uint64_t*>(Op->Header.Args[0]));
       LogMan::Throw::A(Data != nullptr, "Couldn't Map pointer to 0x%lx\n", *GetSrc<uint64_t*>(Op->Header.Args[0]));
       memcpy(GDP, Data, OpSize);

       uint64_t Ret{};
       memcpy(&Ret, Data, Op->Size > 8 ? 8 : Op->Size);
       //LogMan::Msg::D("Loading from guestmem: 0x%lx (%d)", *GetSrc<uint64_t*>(Op->Header.Args[0]), Op->Size);
       //LogMan::Msg::D("\tLoading: 0x%016lx", Ret);
     break;
     }
     case IR::OP_STOREMEM: {
 #define STORE_DATA(x, y) \
     case x: { \
       y *Data = Thread->CTX->MemoryMapper.GetBaseOffset<y *>(*GetSrc<uint64_t*>(Op->Header.Args[0])); \
       LogMan::Throw::A(Data != nullptr, "Couldn't Map pointer to 0x%lx for size %d store\n", *GetSrc<uint64_t*>(Op->Header.Args[0]), x);\
       *Data = *GetSrc<y*>(Op->Header.Args[1]); \
     } \
     break

      auto Op = IROp->C<IR::IROp_StoreMem>();
      //LogMan::Msg::D("Storing guestmem: 0x%lx (%d)", *GetSrc<uint64_t*>(Op->Header.Args[0]), Op->Size);
      //LogMan::Msg::D("\tStoring: 0x%016lx", (uint64_t)*GetSrc<uint64_t*>(Op->Header.Args[1]));

      switch (Op->Size) {
      STORE_DATA(1, uint8_t);
      STORE_DATA(2, uint16_t);
      STORE_DATA(4, uint32_t);
      STORE_DATA(8, uint64_t);
      case 16: {
        void *Mem = Thread->CTX->MemoryMapper.GetPointer<void*>(*GetSrc<uint64_t*>(Op->Header.Args[0]));
        void *Src = GetSrc<void*>(Op->Header.Args[1]);
        memcpy(Mem, Src, 16);
      }
      break;
      default:
        LogMan::Msg::A("Unhandled StoreMem size");
      break;
      }
     #undef STORE_DATA
     break;
     }
     case IR::OP_ADD: {
       auto Op = IROp->C<IR::IROp_Add>();
       uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
       uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

       GD = Src1 + Src2;
     break;
     }
     case IR::OP_SUB: {
       auto Op = IROp->C<IR::IROp_Sub>();
       uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
       uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

       GD = Src1 - Src2;
     break;
     }
     case IR::OP_MUL: {
       auto Op = IROp->C<IR::IROp_Mul>();
       uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
       uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

       switch (OpSize) {
       case 1:
         GD = static_cast<int64_t>(static_cast<int8_t>(Src1)) * static_cast<int64_t>(static_cast<int8_t>(Src2));
       break;
       case 2:
         GD = static_cast<int64_t>(static_cast<int16_t>(Src1)) * static_cast<int64_t>(static_cast<int16_t>(Src2));
       break;
       case 4:
         GD = static_cast<int64_t>(static_cast<int32_t>(Src1)) * static_cast<int64_t>(static_cast<int32_t>(Src2));
       break;
       case 8:
         GD = static_cast<int64_t>(Src1) * static_cast<int64_t>(Src2);
       break;
       case 16: {
         __int128_t Tmp = static_cast<__int128_t>(static_cast<int64_t>(Src1)) * static_cast<__int128_t>(static_cast<int64_t>(Src2));
         memcpy(GDP, &Tmp, 16);
       }
       break;

       default: LogMan::Msg::A("Unknown Mul Size: %d\n", OpSize); break;
       }
     break;
     }
    case IR::OP_MULH: {
      auto Op = IROp->C<IR::IROp_MulH>();
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

      switch (OpSize) {
      case 1: {
        int64_t Tmp = static_cast<int64_t>(static_cast<int8_t>(Src1)) * static_cast<int64_t>(static_cast<int8_t>(Src2));
        GD = Tmp >> 8;
      break;
      }
      case 2: {
        int64_t Tmp = static_cast<int64_t>(static_cast<int16_t>(Src1)) * static_cast<int64_t>(static_cast<int16_t>(Src2));
        GD = Tmp >> 16;
      break;
      }
      case 4: {
        int64_t Tmp = static_cast<int64_t>(static_cast<int32_t>(Src1)) * static_cast<int64_t>(static_cast<int32_t>(Src2));
        GD = Tmp >> 32;
      break;
      }
      case 8: {
        __int128_t Tmp = static_cast<__int128_t>(static_cast<int64_t>(Src1)) * static_cast<__int128_t>(static_cast<int64_t>(Src2));
        GD = Tmp >> 64;
      }
      break;
      default: LogMan::Msg::A("Unknown MulH Size: %d\n", OpSize); break;
      }
    break;
    }
    case IR::OP_UMUL: {
      auto Op = IROp->C<IR::IROp_UMul>();
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

      switch (OpSize) {
      case 1:
        GD = static_cast<uint8_t>(Src1) * static_cast<uint8_t>(Src2);
      break;
      case 2:
        GD = static_cast<uint16_t>(Src1) * static_cast<uint16_t>(Src2);
      break;
      case 4:
        GD = static_cast<uint32_t>(Src1) * static_cast<uint32_t>(Src2);
      break;
      case 8:
        GD = static_cast<uint64_t>(Src1) * static_cast<uint64_t>(Src2);
      break;
      case 16: {
        __uint128_t Tmp = static_cast<__uint128_t>(static_cast<uint64_t>(Src1)) * static_cast<__uint128_t>(static_cast<uint64_t>(Src2));
        memcpy(GDP, &Tmp, 16);
      }
      break;

      default: LogMan::Msg::A("Unknown UMul Size: %d\n", OpSize); break;
      }
    break;
    }
    case IR::OP_UMULH: {
      auto Op = IROp->C<IR::IROp_UMulH>();
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);
      switch (OpSize) {
      case 1:
        GD = static_cast<uint16_t>(Src1) * static_cast<uint16_t>(Src2);
        GD >>= 8;
      break;
      case 2:
        GD = static_cast<uint32_t>(Src1) * static_cast<uint32_t>(Src2);
        GD >>= 16;
      break;
      case 4:
        GD = static_cast<uint64_t>(Src1) * static_cast<uint64_t>(Src2);
        GD >>= 32;
      break;
      case 8: {
        __uint128_t Tmp = static_cast<__uint128_t>(Src1) * static_cast<__uint128_t>(Src2);
        GD = Tmp >> 64;
      }
      break;
      case 16: {
        // XXX: This is incorrect
        __uint128_t Tmp = static_cast<__uint128_t>(Src1) * static_cast<__uint128_t>(Src2);
        GD = Tmp >> 64;
      }
      break;

      default: LogMan::Msg::A("Unknown UMulH Size: %d\n", OpSize); break;
      }
    break;
    }
     case IR::OP_DIV: {
       auto Op = IROp->C<IR::IROp_Div>();
       uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
       uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

       switch (OpSize) {
       case 1:
         GD = static_cast<int64_t>(static_cast<int8_t>(Src1)) / static_cast<int64_t>(static_cast<int8_t>(Src2));
       break;
       case 2:
         GD = static_cast<int64_t>(static_cast<int16_t>(Src1)) / static_cast<int64_t>(static_cast<int16_t>(Src2));
       break;
       case 4:
         GD = static_cast<int64_t>(static_cast<int32_t>(Src1)) / static_cast<int64_t>(static_cast<int32_t>(Src2));
       break;
       case 8:
         GD = static_cast<int64_t>(Src1) / static_cast<int64_t>(Src2);
       break;
       case 16: {
         __int128_t Tmp = *GetSrc<__int128_t*>(Op->Header.Args[0]) / *GetSrc<__int128_t*>(Op->Header.Args[1]);
         memcpy(GDP, &Tmp, 16);
       }
       break;

       default: LogMan::Msg::A("Unknown Mul Size: %d\n", OpSize); break;
       }
     break;
     }

     case IR::OP_UDIV: {
       auto Op = IROp->C<IR::IROp_UDiv>();
       uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
       uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

       switch (OpSize) {
       case 1:
         GD = static_cast<uint64_t>(static_cast<uint8_t>(Src1)) / static_cast<uint64_t>(static_cast<uint8_t>(Src2));
       break;
       case 2:
         GD = static_cast<uint64_t>(static_cast<uint16_t>(Src1)) / static_cast<uint64_t>(static_cast<uint16_t>(Src2));
       break;
       case 4:
         GD = static_cast<uint64_t>(static_cast<uint32_t>(Src1)) / static_cast<uint64_t>(static_cast<uint32_t>(Src2));
       break;
       case 8:
         GD = static_cast<uint64_t>(Src1) / static_cast<uint64_t>(Src2);
       break;
       case 16: {
         __uint128_t Tmp = *GetSrc<__uint128_t*>(Op->Header.Args[0]) / *GetSrc<__uint128_t*>(Op->Header.Args[1]);
         memcpy(GDP, &Tmp, 16);
       }
       break;

       default: LogMan::Msg::A("Unknown Mul Size: %d\n", OpSize); break;
       }
     break;
     }

     case IR::OP_REM: {
       auto Op = IROp->C<IR::IROp_Rem>();
       uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
       uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

       switch (OpSize) {
       case 1:
         GD = static_cast<int64_t>(static_cast<int8_t>(Src1)) % static_cast<int64_t>(static_cast<int8_t>(Src2));
       break;
       case 2:
         GD = static_cast<int64_t>(static_cast<int16_t>(Src1)) % static_cast<int64_t>(static_cast<int16_t>(Src2));
       break;
       case 4:
         GD = static_cast<int64_t>(static_cast<int32_t>(Src1)) % static_cast<int64_t>(static_cast<int32_t>(Src2));
       break;
       case 8:
         GD = static_cast<int64_t>(Src1) % static_cast<int64_t>(Src2);
       break;
       case 16: {
         __int128_t Tmp = *GetSrc<__int128_t*>(Op->Header.Args[0]) % *GetSrc<__int128_t*>(Op->Header.Args[1]);
         memcpy(GDP, &Tmp, 16);
       }
       break;

       default: LogMan::Msg::A("Unknown Mul Size: %d\n", OpSize); break;
       }
     break;
     }

     case IR::OP_UREM: {
       auto Op = IROp->C<IR::IROp_URem>();
       uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
       uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

       switch (OpSize) {
       case 1:
         GD = static_cast<uint64_t>(static_cast<uint8_t>(Src1)) % static_cast<uint64_t>(static_cast<uint8_t>(Src2));
       break;
       case 2:
         GD = static_cast<uint64_t>(static_cast<uint16_t>(Src1)) % static_cast<uint64_t>(static_cast<uint16_t>(Src2));
       break;
       case 4:
         GD = static_cast<uint64_t>(static_cast<uint32_t>(Src1)) % static_cast<uint64_t>(static_cast<uint32_t>(Src2));
       break;
       case 8:
         GD = static_cast<uint64_t>(Src1) % static_cast<uint64_t>(Src2);
       break;
       case 16: {
         __uint128_t Tmp = *GetSrc<__uint128_t*>(Op->Header.Args[0]) % *GetSrc<__uint128_t*>(Op->Header.Args[1]);
         memcpy(GDP, &Tmp, 16);
       }
       break;

       default: LogMan::Msg::A("Unknown Mul Size: %d\n", OpSize); break;
       }
     break;
     }


    case IR::OP_OR: {
      auto Op = IROp->C<IR::IROp_Or>();
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

      GD = Src1 | Src2;
    break;
    }
    case IR::OP_AND: {
      auto Op = IROp->C<IR::IROp_And>();
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

      GD = Src1 & Src2;
    break;
    }
    case IR::OP_XOR: {
      auto Op = IROp->C<IR::IROp_Xor>();
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

      GD = Src1 ^ Src2;
    break;
    }
    case IR::OP_LSHL: {
      auto Op = IROp->C<IR::IROp_Lshl>();
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);
      uint8_t Mask = OpSize * 8 - 1;
      GD = Src1 << (Src2 & Mask);
    break;
    }
    case IR::OP_LSHR: {
      auto Op = IROp->C<IR::IROp_Lshr>();
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);
      uint8_t Mask = OpSize * 8 - 1;
      GD = Src1 >> (Src2 & Mask);
    break;
    }
    case IR::OP_ASHR: {
      auto Op = IROp->C<IR::IROp_Ashr>();
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);
      uint8_t Mask = OpSize * 8 - 1;
      switch (OpSize) {
      case 1:
        GD = static_cast<int8_t>(Src1) >> (Src2 & Mask);
      break;
      case 2:
        GD = static_cast<int16_t>(Src1) >> (Src2 & Mask);
      break;
      case 4:
        GD = static_cast<int32_t>(Src1) >> (Src2 & Mask);
      break;
      case 8:
        GD = static_cast<int64_t>(Src1) >> (Src2 & Mask);
      break;
      default: LogMan::Msg::A("Unknown ASHR Size: %d\n", OpSize); break;
      };
    break;
    }
    case IR::OP_ROR: {
      auto Op = IROp->C<IR::IROp_Ror>();
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);
      auto Ror = [] (auto In, auto R) {
        auto RotateMask = sizeof(In) * 8 - 1;
        R &= RotateMask;
        return (In >> R) | (In << (sizeof(In) * 8 - R));
      };

      switch (OpSize) {
      case 1:
        GD = Ror(static_cast<uint8_t>(Src1), static_cast<uint8_t>(Src2));
      break;
      case 2:
        GD = Ror(static_cast<uint16_t>(Src1), static_cast<uint16_t>(Src2));
      break;
      case 4:
        GD = Ror(static_cast<uint32_t>(Src1), static_cast<uint32_t>(Src2));
      break;
      case 8: {
        GD = Ror(static_cast<uint64_t>(Src1), static_cast<uint64_t>(Src2));
      }
      break;
      default: LogMan::Msg::A("Unknown ROR Size: %d\n", OpSize); break;
      }
    break;
    }
    case IR::OP_ROL: {
      auto Op = IROp->C<IR::IROp_Rol>();
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);
      auto Rol = [] (auto In, auto R) {
        auto RotateMask = sizeof(In) * 8 - 1;
        R &= RotateMask;
        return (In << R) | (In >> (sizeof(In) * 8 - R));
      };

      switch (OpSize) {
      case 1:
        GD = Rol(static_cast<uint8_t>(Src1), static_cast<uint8_t>(Src2));
      break;
      case 2:
        GD = Rol(static_cast<uint16_t>(Src1), static_cast<uint16_t>(Src2));
      break;
      case 4:
        GD = Rol(static_cast<uint32_t>(Src1), static_cast<uint32_t>(Src2));
      break;
      case 8: {
        GD = Rol(static_cast<uint64_t>(Src1), static_cast<uint64_t>(Src2));
      }
      break;
      default: LogMan::Msg::A("Unknown ROL Size: %d\n", OpSize); break;
      }
    break;
    }

    case IR::OP_ZEXT: {
      auto Op = IROp->C<IR::IROp_Zext>();
      LogMan::Throw::A(Op->SrcSize <= 64, "Can't support Zext of size: %ld", Op->SrcSize);
      uint64_t Src = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      if (Op->SrcSize == 64) {
        // Zext 64bit to 128bit
        __uint128_t SrcLarge = Src;
        memcpy(GDP, &SrcLarge, 16);
      }
      else {
        GD = Src & ((1ULL << Op->SrcSize) - 1);
      }
    break;
    }
    case IR::OP_SEXT: {
      auto Op = IROp->C<IR::IROp_Sext>();
      LogMan::Throw::A(Op->SrcSize <= 64, "Can't support Zext of size: %ld", Op->SrcSize);
      switch (Op->SrcSize / 8) {
      case 1:
        GD = *GetSrc<int8_t*>(Op->Header.Args[0]);
      break;
      case 2:
        GD = *GetSrc<int16_t*>(Op->Header.Args[0]);
      break;
      case 4:
        GD = *GetSrc<int32_t*>(Op->Header.Args[0]);
      break;
      case 8:
        GD = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      break;
      default: LogMan::Msg::A("Unknown Sext size: %d", Op->SrcSize / 8);
      }
    break;
    }
    case IR::OP_NEG: {
      auto Op = IROp->C<IR::IROp_Neg>();
      uint64_t Src = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      GD = ~Src;
    break;
    }
    case IR::OP_POPCOUNT: {
      auto Op = IROp->C<IR::IROp_Popcount>();
      uint64_t Src = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      GD = __builtin_popcountl(Src);
    break;
    }
    case IR::OP_FINDLSB: {
      auto Op = IROp->C<IR::IROp_FindLSB>();
      uint64_t Src = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Result = __builtin_ffsll(Src);
      GD = Result - 1;
    break;
    }
    case IR::OP_FINDMSB: {
      auto Op = IROp->C<IR::IROp_FindMSB>();
      uint64_t Src = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Result = Op->Header.Size * 8 - __builtin_clzll(Src);
      GD = Result;
    break;
    }

    case IR::OP_SELECT: {
      auto Op = IROp->C<IR::IROp_Select>();
      bool CompResult = false;
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

      uint64_t ArgTrue = *GetSrc<uint64_t*>(Op->Header.Args[2]);
      uint64_t ArgFalse = *GetSrc<uint64_t*>(Op->Header.Args[3]);

      switch (Op->Cond) {
      case FEXCore::IR::COND_EQ:
        CompResult = Src1 == Src2;
      break;
      case FEXCore::IR::COND_NEQ:
        CompResult = Src1 != Src2;
      break;
      case FEXCore::IR::COND_GE:
        CompResult = Src1 >= Src2;
      break;
      case FEXCore::IR::COND_LT:
        CompResult = Src1 < Src2;
      break;
      case FEXCore::IR::COND_GT:
        CompResult = Src1 > Src2;
      break;
      case FEXCore::IR::COND_LE:
        CompResult = Src1 <= Src2;
      break;
      case FEXCore::IR::COND_CS:
      case FEXCore::IR::COND_CC:
      case FEXCore::IR::COND_MI:
      case FEXCore::IR::COND_PL:
      case FEXCore::IR::COND_VS:
      case FEXCore::IR::COND_VC:
      case FEXCore::IR::COND_HI:
      case FEXCore::IR::COND_LS:
      default:
      LogMan::Msg::A("Unsupported compare type");
      break;
      }
      GD = CompResult ? ArgTrue : ArgFalse;
    break;
    }
    case IR::OP_BFI: {
      auto Op = IROp->C<IR::IROp_Bfi>();
      uint64_t SourceMask = (1ULL << Op->Width) - 1;
      if (Op->Width == 64)
        SourceMask = ~0ULL;
      uint64_t DestMask = ~(SourceMask << Op->lsb);
      uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
      uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);
      uint64_t Res = (Src1 & DestMask) | ((Src2 & SourceMask) << Op->lsb);
      GD = Res;
    break;
    }
    case IR::OP_BFE: {
      auto Op = IROp->C<IR::IROp_Bfe>();
      LogMan::Throw::A(OpSize <= 16, "OpSize is too large for BFE: %d", OpSize);
      if (OpSize == 16) {
        LogMan::Throw::A(Op->Width <= 64, "Can't extract width of %d", Op->Width);
        __uint128_t SourceMask = (1ULL << Op->Width) - 1;
        if (Op->Width == 64)
          SourceMask = ~0ULL;
        SourceMask <<= Op->lsb;
        __uint128_t Src = (*GetSrc<__uint128_t*>(Op->Header.Args[0]) & SourceMask) >> Op->lsb;
        memcpy(GDP, &Src, OpSize);
      }
      else {
        uint64_t SourceMask = (1ULL << Op->Width) - 1;
        if (Op->Width == 64)
          SourceMask = ~0ULL;
        SourceMask <<= Op->lsb;
        uint64_t Src = *GetSrc<uint64_t*>(Op->Header.Args[0]);
        GD = (Src & SourceMask) >> Op->lsb;
      }
    break;
    }
    case IR::OP_PRINT: {
      auto Op = IROp->C<IR::IROp_Print>();

      if (OpSize <= 8) {
        uint64_t Src = *GetSrc<uint64_t*>(Op->Header.Args[0]);
        LogMan::Msg::I(">>>> Value in Arg: 0x%lx, %ld", Src, Src);
      }
      else if (OpSize == 16) {
        __uint128_t Src = *GetSrc<__uint128_t*>(Op->Header.Args[0]);
        uint64_t Src0 = Src;
        uint64_t Src1 = Src >> 64;
        LogMan::Msg::I(">>>> Value[0] in Arg: 0x%lx, %ld", Src0, Src0);
        LogMan::Msg::I("     Value[1] in Arg: 0x%lx, %ld", Src1, Src1);
      }
      else
        LogMan::Msg::A("Unknown value size: %d", OpSize);
    break;
    }
    case IR::OP_CPUID: {
      auto Op = IROp->C<IR::IROp_CPUID>();
      uint64_t *DstPtr = GetDest<uint64_t*>(*WrapperOp);
      uint64_t Arg = *GetSrc<uint64_t*>(Op->Header.Args[0]);

      auto Results = CTX->CPUID.RunFunction(Arg);
      memcpy(DstPtr, &Results.Res, sizeof(uint32_t) * 4);
    break;
    }
    case IR::OP_EXTRACTELEMENT: {
      auto ExtractElementOp = IROp->C<IR::IROp_ExtractElement>();

      uintptr_t DstPtr = GetDest<uintptr_t>(*WrapperOp);
      uintptr_t SrcPtr = GetSrc<uintptr_t>(ExtractElementOp->Header.Args[0]);

      // Offset to the element offset
      SrcPtr += IROp->Size * ExtractElementOp->Idx;
      memcpy(reinterpret_cast<void*>(DstPtr), reinterpret_cast<void*>(SrcPtr), IROp->Size);
    break;
    }
    case IR::OP_CAS: {
      auto Op = IROp->C<IR::IROp_CAS>();
      auto Size = OpSize;
      switch (Size) {
      case 1: {
        std::atomic<uint8_t> *Data = Thread->CTX->MemoryMapper.GetPointer<std::atomic<uint8_t> *>(*GetSrc<uint64_t*>(Op->Header.Args[2]));
        LogMan::Throw::A(Data != nullptr, "Couldn't Map pointer to 0x%lx\n", *GetSrc<uint8_t*>(Op->Header.Args[2]));

        uint8_t Src1 = *GetSrc<uint8_t*>(Op->Header.Args[0]);
        uint8_t Src2 = *GetSrc<uint8_t*>(Op->Header.Args[1]);

        uint8_t Expected = Src1;
        bool Result = Data->compare_exchange_strong(Expected, Src2);
        GD = Result ? Src1 : Expected;
      break;
      }
      case 2: {
        std::atomic<uint16_t> *Data = Thread->CTX->MemoryMapper.GetPointer<std::atomic<uint16_t> *>(*GetSrc<uint64_t*>(Op->Header.Args[2]));
        LogMan::Throw::A(Data != nullptr, "Couldn't Map pointer to 0x%lx\n", *GetSrc<uint16_t*>(Op->Header.Args[2]));

        uint16_t Src1 = *GetSrc<uint16_t*>(Op->Header.Args[0]);
        uint16_t Src2 = *GetSrc<uint16_t*>(Op->Header.Args[1]);

        uint16_t Expected = Src1;
        bool Result = Data->compare_exchange_strong(Expected, Src2);
        GD = Result ? Src1 : Expected;
      break;
      }
      case 4: {
        std::atomic<uint32_t> *Data = Thread->CTX->MemoryMapper.GetPointer<std::atomic<uint32_t> *>(*GetSrc<uint64_t*>(Op->Header.Args[2]));
        LogMan::Throw::A(Data != nullptr, "Couldn't Map pointer to 0x%lx\n", *GetSrc<uint32_t*>(Op->Header.Args[2]));

        uint32_t Src1 = *GetSrc<uint32_t*>(Op->Header.Args[0]);
        uint32_t Src2 = *GetSrc<uint32_t*>(Op->Header.Args[1]);

        uint32_t Expected = Src1;
        bool Result = Data->compare_exchange_strong(Expected, Src2);
        GD = Result ? Src1 : Expected;
      break;
      }
      case 8: {
        std::atomic<uint64_t> *Data = Thread->CTX->MemoryMapper.GetPointer<std::atomic<uint64_t> *>(*GetSrc<uint64_t*>(Op->Header.Args[2]));
        LogMan::Throw::A(Data != nullptr, "Couldn't Map pointer to 0x%lx\n", *GetSrc<uint64_t*>(Op->Header.Args[2]));

        uint64_t Src1 = *GetSrc<uint64_t*>(Op->Header.Args[0]);
        uint64_t Src2 = *GetSrc<uint64_t*>(Op->Header.Args[1]);

        uint64_t Expected = Src1;
        bool Result = Data->compare_exchange_strong(Expected, Src2);
        GD = Result ? Src1 : Expected;
      break;
      }
      default: LogMan::Msg::A("Unknown CAS size: %d", Size); break;
      }
    break;
    }
    case IR::OP_REV: {
      auto Op = IROp->C<IR::IROp_Rev>();
      switch (OpSize) {
      case 2: GD = __builtin_bswap16(*GetSrc<uint16_t*>(Op->Header.Args[0])); break;
      case 4: GD = __builtin_bswap32(*GetSrc<uint32_t*>(Op->Header.Args[0])); break;
      case 8: GD = __builtin_bswap64(*GetSrc<uint64_t*>(Op->Header.Args[0])); break;
      default: LogMan::Msg::A("Unknown REV size: %d", OpSize); break;
      }
    break;
    }

    case IR::OP_CYCLECOUNTER: {
#ifdef DEBUG_CYCLES
      GD = 0;
#else
      timespec time;
      clock_gettime(CLOCK_REALTIME, &time);
      GD = time.tv_nsec + time.tv_sec * 1000000000;
#endif
    break;
    }
    // Vector ops
    case IR::OP_CREATEVECTOR2: {
      auto Op = IROp->C<IR::IROp_CreateVector2>();
      LogMan::Throw::A(OpSize <= 16, "Can't handle a vector of size: %d", OpSize);
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];
      uint8_t ElementSize = OpSize / 2;
#define CREATE_VECTOR(elementsize, type) \
  case elementsize: { \
    auto *Dst_d = reinterpret_cast<type*>(Tmp); \
    auto *Src1_d = reinterpret_cast<type*>(Src1); \
    auto *Src2_d = reinterpret_cast<type*>(Src2); \
    Dst_d[0] = *Src1_d; \
    Dst_d[1] = *Src2_d; \
  break; \
  }
    switch (ElementSize) {
    CREATE_VECTOR(1, uint8_t);
    CREATE_VECTOR(2, uint16_t);
    CREATE_VECTOR(4, uint32_t);
    CREATE_VECTOR(8, uint64_t);
    default: LogMan::Msg::A("Unknown Element Size: %d", ElementSize); break;
    }
#undef CREATE_VECTOR
    memcpy(GDP, Tmp, OpSize);

    break;
    }
    case IR::OP_SPLATVECTOR4:
    case IR::OP_SPLATVECTOR3:
    case IR::OP_SPLATVECTOR2: {
      auto Op = IROp->C<IR::IROp_SplatVector2>();
      LogMan::Throw::A(OpSize <= 16, "Can't handle a vector of size: %d", OpSize);
      void *Src = GetSrc<void*>(Op->Header.Args[0]);
      uint8_t Tmp[16];
      uint8_t Elements = 0;

      switch (Op->Header.Op) {
      case IR::OP_SPLATVECTOR4: Elements = 4; break;
      case IR::OP_SPLATVECTOR3: Elements = 3; break;
      case IR::OP_SPLATVECTOR2: Elements = 2; break;
      default: LogMan::Msg::A("Uknown Splat size"); break;
      }

      uint8_t ElementSize = OpSize / Elements;
#define CREATE_VECTOR(elementsize, type) \
  case elementsize: { \
    auto *Dst_d = reinterpret_cast<type*>(Tmp); \
    auto *Src_d = reinterpret_cast<type*>(Src); \
    for (uint8_t i = 0; i < Elements; ++i) \
      Dst_d[i] = *Src_d;\
    break; \
    }
    switch (ElementSize) {
    CREATE_VECTOR(1, uint8_t);
    CREATE_VECTOR(2, uint16_t);
    CREATE_VECTOR(4, uint32_t);
    CREATE_VECTOR(8, uint64_t);
    default: LogMan::Msg::A("Unknown Element Size: %d", ElementSize); break;
    }
#undef CREATE_VECTOR
    memcpy(GDP, Tmp, OpSize);

    break;
    }

    case IR::OP_VOR: {
      auto Op = IROp->C<IR::IROp_VOr>();
      __uint128_t Src1 = *GetSrc<__uint128_t*>(Op->Header.Args[0]);
      __uint128_t Src2 = *GetSrc<__uint128_t*>(Op->Header.Args[1]);

      __uint128_t Dst = Src1 | Src2;
      memcpy(GDP, &Dst, 16);
    break;
    }
    case IR::OP_VXOR: {
      auto Op = IROp->C<IR::IROp_VXor>();
      __uint128_t Src1 = *GetSrc<__uint128_t*>(Op->Header.Args[0]);
      __uint128_t Src2 = *GetSrc<__uint128_t*>(Op->Header.Args[1]);

      __uint128_t Dst = Src1 ^ Src2;
      memcpy(GDP, &Dst, 16);
    break;
    }
#define DO_VECTOR_OP(size, type, func)              \
  case size: {                                      \
  auto *Dst_d  = reinterpret_cast<type*>(Tmp);  \
  auto *Src1_d = reinterpret_cast<type*>(Src1); \
  auto *Src2_d = reinterpret_cast<type*>(Src2); \
  for (uint8_t i = 0; i < Elements; ++i) {          \
    Dst_d[i] = func(Src1_d[i], Src2_d[i]);          \
  }                                                 \
  break;                                            \
  }
#define DO_VECTOR_SCALAR_OP(size, type, func)\
  case size: {                                      \
  auto *Dst_d  = reinterpret_cast<type*>(Tmp);  \
  auto *Src1_d = reinterpret_cast<type*>(Src1); \
  auto *Src2_d = reinterpret_cast<type*>(Src2); \
  for (uint8_t i = 0; i < Elements; ++i) {          \
    Dst_d[i] = func(Src1_d[i], *Src2_d);          \
  }                                                 \
  break;                                            \
  }

    case IR::OP_VADD: {
      auto Op = IROp->C<IR::IROp_VAdd>();
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];

      uint8_t Elements = Op->RegisterSize / Op->ElementSize;

      auto Func = [](auto a, auto b) { return a + b; };
      switch (Op->ElementSize) {
      DO_VECTOR_OP(1, uint8_t,  Func)
      DO_VECTOR_OP(2, uint16_t, Func)
      DO_VECTOR_OP(4, uint32_t, Func)
      DO_VECTOR_OP(8, uint64_t, Func)
      default: LogMan::Msg::A("Unknown Element Size: %d", Op->ElementSize); break;
      }
      memcpy(GDP, Tmp, Op->RegisterSize);
    break;
    }
    case IR::OP_VSUB: {
      auto Op = IROp->C<IR::IROp_VSub>();
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];

      uint8_t Elements = Op->RegisterSize / Op->ElementSize;

      auto Func = [](auto a, auto b) { return a - b; };
      switch (Op->ElementSize) {
      DO_VECTOR_OP(1, uint8_t,  Func)
      DO_VECTOR_OP(2, uint16_t, Func)
      DO_VECTOR_OP(4, uint32_t, Func)
      DO_VECTOR_OP(8, uint64_t, Func)
      default: LogMan::Msg::A("Unknown Element Size: %d", Op->ElementSize); break;
      }
      memcpy(GDP, Tmp, Op->RegisterSize);
    break;
    }
    case IR::OP_VUMIN: {
      auto Op = IROp->C<IR::IROp_VUMin>();
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];

      uint8_t Elements = Op->RegisterSize / Op->ElementSize;
      auto Func = [](auto a, auto b) { return std::min(a, b); };

      switch (Op->ElementSize) {
      DO_VECTOR_OP(1, uint8_t,  Func)
      DO_VECTOR_OP(2, uint16_t, Func)
      DO_VECTOR_OP(4, uint32_t, Func)
      DO_VECTOR_OP(8, uint64_t, Func)
      default: LogMan::Msg::A("Unknown Element Size: %d", Op->ElementSize); break;
      }
      memcpy(GDP, Tmp, Op->RegisterSize);
    break;
    }
    case IR::OP_VSMIN: {
      auto Op = IROp->C<IR::IROp_VSMin>();
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];

      uint8_t Elements = Op->RegisterSize / Op->ElementSize;
      auto Func = [](auto a, auto b) { return std::min(a, b); };

      switch (Op->ElementSize) {
      DO_VECTOR_OP(1, int8_t,  Func)
      DO_VECTOR_OP(2, int16_t, Func)
      DO_VECTOR_OP(4, int32_t, Func)
      DO_VECTOR_OP(8, int64_t, Func)
      default: LogMan::Msg::A("Unknown Element Size: %d", Op->ElementSize); break;
      }
      memcpy(GDP, Tmp, Op->RegisterSize);
    break;
    }
    case IR::OP_VUSHL: {
      auto Op = IROp->C<IR::IROp_VUShl>();
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];

      uint8_t Elements = Op->RegisterSize / Op->ElementSize;
      auto Func = [](auto a, auto b) { return a << b; };

      switch (Op->ElementSize) {
      DO_VECTOR_OP(1, uint8_t,  Func)
      DO_VECTOR_OP(2, uint16_t, Func)
      DO_VECTOR_OP(4, uint32_t, Func)
      DO_VECTOR_OP(8, uint64_t, Func)
      default: LogMan::Msg::A("Unknown Element Size: %d", Op->ElementSize); break;
      }
      memcpy(GDP, Tmp, Op->RegisterSize);
    break;
    }

    case IR::OP_VUSHLS: {
      auto Op = IROp->C<IR::IROp_VUShlS>();
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];

      uint8_t Elements = Op->RegisterSize / Op->ElementSize;
      auto Func = [](auto a, auto b) { return a << b; };

      switch (Op->ElementSize) {
      DO_VECTOR_SCALAR_OP(1, uint8_t, Func)
      DO_VECTOR_SCALAR_OP(2, uint16_t, Func)
      DO_VECTOR_SCALAR_OP(4, uint32_t, Func)
      DO_VECTOR_SCALAR_OP(8, uint64_t, Func)
      DO_VECTOR_SCALAR_OP(16, __uint128_t, Func)
      default: LogMan::Msg::A("Unknown Element Size: %d", Op->ElementSize); break;
      }
      memcpy(GDP, Tmp, Op->RegisterSize);
    break;
    }

    case IR::OP_VUSHR: {
      auto Op = IROp->C<IR::IROp_VUShr>();
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];

      uint8_t Elements = Op->RegisterSize / Op->ElementSize;
      auto Func = [](auto a, auto b) { return a >> b; };

      switch (Op->ElementSize) {
      DO_VECTOR_OP(1, uint8_t,  Func)
      DO_VECTOR_OP(2, uint16_t, Func)
      DO_VECTOR_OP(4, uint32_t, Func)
      DO_VECTOR_OP(8, uint64_t, Func)
      default: LogMan::Msg::A("Unknown Element Size: %d", Op->ElementSize); break;
      }
      memcpy(GDP, Tmp, Op->RegisterSize);
    break;
    }

    case IR::OP_VZIP2:
    case IR::OP_VZIP: {
      auto Op = IROp->C<IR::IROp_VZip>();
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];
      uint8_t Elements = Op->RegisterSize / Op->ElementSize;
      uint8_t BaseOffset = IROp->Op == IR::OP_VZIP2 ? (Elements / 2) : 0;
      Elements >>= 1;

      switch (Op->ElementSize) {
      case 1: {
        auto *Dst_d  = reinterpret_cast<uint8_t*>(Tmp);
        auto *Src1_d = reinterpret_cast<uint8_t*>(Src1);
        auto *Src2_d = reinterpret_cast<uint8_t*>(Src2);
        for (unsigned i = 0; i < Elements; ++i) {
          Dst_d[i*2] = Src1_d[BaseOffset + i];
          Dst_d[i*2+1] = Src2_d[BaseOffset + i];
        }
      break;
      }
      case 2: {
        auto *Dst_d  = reinterpret_cast<uint16_t*>(Tmp);
        auto *Src1_d = reinterpret_cast<uint16_t*>(Src1);
        auto *Src2_d = reinterpret_cast<uint16_t*>(Src2);
        for (unsigned i = 0; i < Elements; ++i) {
          Dst_d[i*2] = Src1_d[BaseOffset + i];
          Dst_d[i*2+1] = Src2_d[BaseOffset + i];
        }
      break;
      }
      case 4: {
        auto *Dst_d  = reinterpret_cast<uint32_t*>(Tmp);
        auto *Src1_d = reinterpret_cast<uint32_t*>(Src1);
        auto *Src2_d = reinterpret_cast<uint32_t*>(Src2);
        for (unsigned i = 0; i < Elements; ++i) {
          Dst_d[i*2] = Src1_d[BaseOffset + i];
          Dst_d[i*2+1] = Src2_d[BaseOffset + i];
        }
      break;
      }
      case 8: {
        auto *Dst_d  = reinterpret_cast<uint64_t*>(Tmp);
        auto *Src1_d = reinterpret_cast<uint64_t*>(Src1);
        auto *Src2_d = reinterpret_cast<uint64_t*>(Src2);
        for (unsigned i = 0; i < Elements; ++i) {
          Dst_d[i*2] = Src1_d[BaseOffset + i];
          Dst_d[i*2+1] = Src2_d[BaseOffset + i];
        }
      break;
      }
      default: LogMan::Msg::A("Unknown Element Size: %d", Op->ElementSize); break;
      }

      memcpy(GDP, Tmp, Op->RegisterSize);
    break;
    }

    case IR::OP_VINSELEMENT: {
      auto Op = IROp->C<IR::IROp_VInsElement>();
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];

      // Copy src1 in to dest
      memcpy(Tmp, Src1, Op->RegisterSize);
      switch (Op->ElementSize) {
      case 1: {
        auto *Dst_d  = reinterpret_cast<uint8_t*>(Tmp);
        auto *Src2_d = reinterpret_cast<uint8_t*>(Src2);
        Dst_d[Op->DestIdx] = Src2_d[Op->SrcIdx];
      break;
      }
      case 2: {
        auto *Dst_d  = reinterpret_cast<uint16_t*>(Tmp);
        auto *Src2_d = reinterpret_cast<uint16_t*>(Src2);
        Dst_d[Op->DestIdx] = Src2_d[Op->SrcIdx];
      break;
      }
      case 4: {
        auto *Dst_d  = reinterpret_cast<uint32_t*>(Tmp);
        auto *Src2_d = reinterpret_cast<uint32_t*>(Src2);
        Dst_d[Op->DestIdx] = Src2_d[Op->SrcIdx];
      break;
      }
      case 8: {
        auto *Dst_d  = reinterpret_cast<uint64_t*>(Tmp);
        auto *Src2_d = reinterpret_cast<uint64_t*>(Src2);
        Dst_d[Op->DestIdx] = Src2_d[Op->SrcIdx];
      break;
      }
      default: LogMan::Msg::A("Unknown Element Size: %d", Op->ElementSize); break;
      };
      memcpy(GDP, Tmp, Op->RegisterSize);
    break;
    }
    case IR::OP_VCMPEQ: {
      auto Op = IROp->C<IR::IROp_VCMPEQ>();
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];

      uint8_t Elements = Op->RegisterSize / Op->ElementSize;
      auto Func = [](auto a, auto b) { return a == b ? ~0ULL : 0; };

      switch (Op->ElementSize) {
      DO_VECTOR_OP(1, uint8_t,   Func)
      DO_VECTOR_OP(2, uint16_t,  Func)
      DO_VECTOR_OP(4, uint32_t,  Func)
      DO_VECTOR_OP(8, uint64_t,  Func)
      default: LogMan::Msg::A("Unknown Element Size: %d", Op->ElementSize); break;
      }

      memcpy(GDP, Tmp, Op->RegisterSize);
    break;
    }
    case IR::OP_VCMPGT: {
      auto Op = IROp->C<IR::IROp_VCMPGT>();
      void *Src1 = GetSrc<void*>(Op->Header.Args[0]);
      void *Src2 = GetSrc<void*>(Op->Header.Args[1]);
      uint8_t Tmp[16];

      uint8_t Elements = Op->RegisterSize / Op->ElementSize;
      auto Func = [](auto a, auto b) { return a > b ? ~0ULL : 0; };

      switch (Op->ElementSize) {
      DO_VECTOR_OP(1, int8_t,   Func)
      DO_VECTOR_OP(2, int16_t,  Func)
      DO_VECTOR_OP(4, int32_t,  Func)
      DO_VECTOR_OP(8, int64_t,  Func)
      default: LogMan::Msg::A("Unknown Element Size: %d", Op->ElementSize); break;
      }

      memcpy(GDP, Tmp, Op->RegisterSize);
    break;
    }

    case IR::OP_LUDIV: {
      auto Op = IROp->C<IR::IROp_LUDiv>();
      // Each source is OpSize in size
      // So you can have up to a 128bit divide from x86-64
      auto Size = OpSize;
      switch (Size) {
      case 4: {
        uint32_t SrcLow = *GetSrc<uint32_t*>(Op->Header.Args[0]);
        uint32_t SrcHigh = *GetSrc<uint32_t*>(Op->Header.Args[1]);
        uint32_t Divisor = *GetSrc<uint32_t*>(Op->Header.Args[2]);
        uint64_t Source = (static_cast<uint64_t>(SrcHigh) << 32) | SrcLow;
        uint64_t Res = Source / Divisor;

        // We only store the lower bits of the result
        GD = static_cast<uint32_t>(Res);
      break;
      }

      case 8: {
        uint64_t SrcLow = *GetSrc<uint64_t*>(Op->Header.Args[0]);
        uint64_t SrcHigh = *GetSrc<uint64_t*>(Op->Header.Args[1]);
        uint64_t Divisor = *GetSrc<uint64_t*>(Op->Header.Args[2]);
        __uint128_t Source = (static_cast<__uint128_t>(SrcHigh) << 64) | SrcLow;
        __uint128_t Res = Source / Divisor;

        // We only store the lower bits of the result
        memcpy(GDP, &Res, Size);
      break;
      }
      default: LogMan::Msg::A("Unknown LUDIV Size: %d", Size); break;
      }
    break;
    }
    case IR::OP_LDIV: {
      auto Op = IROp->C<IR::IROp_LDiv>();
      // Each source is OpSize in size
      // So you can have up to a 128bit divide from x86-64
      auto Size = OpSize;
      switch (Size) {
      case 4: {
        uint32_t SrcLow = *GetSrc<uint32_t*>(Op->Header.Args[0]);
        uint32_t SrcHigh = *GetSrc<uint32_t*>(Op->Header.Args[1]);
        int32_t Divisor = *GetSrc<uint32_t*>(Op->Header.Args[2]);
        int64_t Source = (static_cast<uint64_t>(SrcHigh) << 32) | SrcLow;
        int64_t Res = Source / Divisor;

        // We only store the lower bits of the result
        GD = static_cast<int32_t>(Res);
      break;
      }
      case 8: {
        uint64_t SrcLow = *GetSrc<uint64_t*>(Op->Header.Args[0]);
        uint64_t SrcHigh = *GetSrc<uint64_t*>(Op->Header.Args[1]);
        int64_t Divisor = *GetSrc<int64_t*>(Op->Header.Args[2]);
        __int128_t Source = (static_cast<__int128_t>(SrcHigh) << 64) | SrcLow;
        __int128_t Res = Source / Divisor;

        // We only store the lower bits of the result
        memcpy(GDP, &Res, Size);
      break;
      }
      default: LogMan::Msg::A("Unknown LDIV Size: %d", Size); break;
      }
    break;
    }
    case IR::OP_LUREM: {
      auto Op = IROp->C<IR::IROp_LURem>();
      // Each source is OpSize in size
      // So you can have up to a 128bit Remainder from x86-64
      auto Size = OpSize;
      switch (Size) {

      case 4: {
        uint32_t SrcLow = *GetSrc<uint32_t*>(Op->Header.Args[0]);
        uint32_t SrcHigh = *GetSrc<uint32_t*>(Op->Header.Args[1]);
        uint32_t Divisor = *GetSrc<uint32_t*>(Op->Header.Args[2]);
        uint64_t Source = (static_cast<uint64_t>(SrcHigh) << 32) | SrcLow;
        uint64_t Res = Source % Divisor;

        // We only store the lower bits of the result
        GD = static_cast<uint32_t>(Res);
      break;
      }

      case 8: {
        uint64_t SrcLow = *GetSrc<uint64_t*>(Op->Header.Args[0]);
        uint64_t SrcHigh = *GetSrc<uint64_t*>(Op->Header.Args[1]);
        uint64_t Divisor = *GetSrc<uint64_t*>(Op->Header.Args[2]);
        __uint128_t Source = (static_cast<__uint128_t>(SrcHigh) << 64) | SrcLow;
        __uint128_t Res = Source % Divisor;
        // We only store the lower bits of the result
        memcpy(GDP, &Res, Size);
      break;
      }
      default: LogMan::Msg::A("Unknown LUREM Size: %d", Size); break;
      }
    break;
    }

    case IR::OP_LREM: {
      auto Op = IROp->C<IR::IROp_LRem>();
      // Each source is OpSize in size
      // So you can have up to a 128bit Remainder from x86-64
      auto Size = OpSize;
      switch (Size) {
      case 4: {
        uint32_t SrcLow = *GetSrc<uint32_t*>(Op->Header.Args[0]);
        uint32_t SrcHigh = *GetSrc<uint32_t*>(Op->Header.Args[1]);
        int32_t Divisor = *GetSrc<uint32_t*>(Op->Header.Args[2]);
        int64_t Source = (static_cast<uint64_t>(SrcHigh) << 32) | SrcLow;
        int64_t Res = Source % Divisor;

        // We only store the lower bits of the result
        GD = static_cast<int32_t>(Res);
      break;
      }

      case 8: {
        uint64_t SrcLow = *GetSrc<uint64_t*>(Op->Header.Args[0]);
        uint64_t SrcHigh = *GetSrc<uint64_t*>(Op->Header.Args[1]);
        int64_t Divisor = *GetSrc<int64_t*>(Op->Header.Args[2]);
        __int128_t Source = (static_cast<__int128_t>(SrcHigh) << 64) | SrcLow;
        __int128_t Res = Source % Divisor;
        // We only store the lower bits of the result
        memcpy(GDP, &Res, Size);
      break;
      }
      default: LogMan::Msg::A("Unknown LREM Size: %d", Size); break;
      }
    break;
    }

     default:
       LogMan::Msg::A("Unknown IR Op: %d(%s)", IROp->Op, FEXCore::IR::GetName(IROp->Op).data());
     break;
     }
     ++Begin;
   }

   Thread->Stats.InstructionsExecuted.fetch_add(DebugData->second.GuestInstructionCount);
}

FEXCore::CPU::CPUBackend *CreateInterpreterCore(FEXCore::Context::Context *ctx) {
  return new InterpreterCore(ctx);
}

}