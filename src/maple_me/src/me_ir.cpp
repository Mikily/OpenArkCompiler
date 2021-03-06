/*
 * Copyright (c) [2019-2020] Huawei Technologies Co.,Ltd.All rights reserved.
 *
 * OpenArkCompiler is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *
 *     http://license.coscl.org.cn/MulanPSL
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
 * FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v1 for more details.
 */
#include "me_ir.h"
#include "opcode_info.h"
#include "irmap.h"
#include "me_irmap.h"
#include "printing.h"
#include "me_ssa.h"
#include "mir_function.h"
#include "global_tables.h"

namespace {
  constexpr int32_t kDefaultPrintIndentNum = 5;
}

namespace maple {
bool MeExpr::IsTheSameWorkcand(const MeExpr &expr) const {
  ASSERT((exprID != -1 || expr.GetExprID() != -1), "at least one of them should not be none initialized");
  if (exprID == expr.GetExprID()) {
    return true;
  }
  if (op != expr.GetOp()) {
    return false;
  }
  if (IsPrimitiveFloat(primType) != IsPrimitiveFloat(expr.GetPrimType())) {
    return false;
  }
  if (GetPrimTypeSize(primType) != GetPrimTypeSize(expr.GetPrimType())) {
    return false;
  }
  if (op == OP_cvt && primType != expr.GetPrimType()) {
    // exclude cvt for different return type
    return false;
  }
  if (op == OP_sext &&
      static_cast<const OpMeExpr*>(this)->GetBitsSize() != static_cast<const OpMeExpr &>(expr).GetBitsSize()) {
    return false;
  }
  if (op == OP_resolveinterfacefunc || op == OP_resolvevirtualfunc || op == OP_iaddrof) {
    if (static_cast<const OpMeExpr*>(this)->GetFieldID() != static_cast<const OpMeExpr &>(expr).GetFieldID()) {
      return false;
    }
  }
  return IsUseSameSymbol(expr);
}

void MeExpr::UpdateDepth() {
  uint8 maxDepth = 0;
  for (int32 i = 0; i < GetNumOpnds(); ++i) {
    MeExpr *opnd = GetOpnd(i);
    if (opnd == nullptr) {
      continue;
    }
    uint8 curDepth = opnd->GetDepth();
    if (curDepth > maxDepth) {
      maxDepth = curDepth;
    }
  }
  ASSERT(maxDepth < UINT8_MAX, "runtime check error");
  depth = static_cast<uint8>(maxDepth + 1);
}

// get the definition of this
// for example:
// v2 = x + b
// v1 = v2
// then v1->ResolveMeExprValue() returns x+b
MeExpr *MeExpr::ResolveMeExprValue() {
  MeExpr *cmpOpLocal = this;
  while (cmpOpLocal != nullptr && cmpOpLocal->GetMeOp() == kMeOpVar) {
    auto *varCmpOp = static_cast<VarMeExpr*>(cmpOpLocal);
    cmpOpLocal = nullptr;
    if (varCmpOp->GetDefBy() == kDefByStmt) {
      cmpOpLocal = static_cast<DassignMeStmt*>(varCmpOp->GetDefStmt())->GetRHS();
    } else if (varCmpOp->GetDefBy() == kDefByChi) {
      ChiMeNode &defchi = varCmpOp->GetDefChi();
      MeStmt *base = defchi.GetBase();
      if (base->GetOp() == OP_maydassign) {
        cmpOpLocal = static_cast<MaydassignMeStmt*>(base)->GetRHS();
      }
    }
  }
  return cmpOpLocal;
}

bool VarMeExpr::IsSameVariableValue(const VarMeExpr &expr) const {
  if (&expr == this) {
    return true;
  }

  if (GetMeOp() == kMeOpVar && GetDefBy() == kDefByStmt && GetDefStmt()->GetOp() == OP_dassign) {
    auto *stmt = static_cast<DassignMeStmt*>(GetDefStmt());
    if (stmt->GetRHS() == &expr) {
      return true;
    }
  }

  return MeExpr::IsSameVariableValue(expr);
}

bool RegMeExpr::IsSameVariableValue(const VarMeExpr &expr) const {
  if (static_cast<const MeExpr *>(&expr) == this) {
    return true;
  }

  if (GetMeOp() == kMeOpReg && GetDefBy() == kDefByStmt && GetDefStmt()->GetOp() == OP_regassign) {
    auto *stmt = static_cast<RegassignMeStmt*>(GetDefStmt());
    if (stmt->GetRHS() == &expr) {
      return true;
    }
  }

  return MeExpr::IsSameVariableValue(expr);
}

// get the definition VarMeExpr of this
// for expample:
// v2 = v3
// v1 = v2
// this = v1
// this->ResolveVarMeValue() returns v3;
// if no resolved VarMeExpr, return this
VarMeExpr &VarMeExpr::ResolveVarMeValue() {
  VarMeExpr *cmpop0 = this;
  while (true) {
    if (cmpop0->defBy != kDefByStmt) {
      break;
    }

    auto *defStmt = static_cast<DassignMeStmt*>(cmpop0->def.defStmt);
    MeExpr *expr = defStmt->GetRHS();
    if (expr->GetMeOp() != kMeOpVar) {
      break;
    }

    cmpop0 = static_cast<VarMeExpr*>(expr);
  }
  return *cmpop0;
}

// *this can be any node, but *v must be VarMeExpr
bool MeExpr::IsSameVariableValue(const VarMeExpr &expr) const {
  if (&expr == this) {
    return true;
  }
  // look up v's value
  if (expr.GetDefBy() == kDefByStmt && expr.GetDefStmt()->GetOp() == OP_dassign) {
    auto *stmt = static_cast<DassignMeStmt*>(expr.GetDefStmt());
    if (stmt->GetRHS() == this) {
      return true;
    }
  }
  return false;
}

// return true if the expression could throw exception; needs to be in sync with
// BaseNode::MayThrowException()
bool MeExpr::CouldThrowException() const {
  if (kOpcodeInfo.MayThrowException(op)) {
    if (op != OP_array) {
      return true;
    }
    if (static_cast<const NaryMeExpr*>(this)->GetBoundCheck()) {
      return true;
    }
  } else if (op == OP_intrinsicop) {
    if (static_cast<const NaryMeExpr*>(this)->GetIntrinsic() == INTRN_JAVA_ARRAY_LENGTH) {
      return true;
    }
  }
  for (int32 i = 0; i < GetNumOpnds(); ++i) {
    if (GetOpnd(i)->CouldThrowException()) {
      return true;
    }
  }
  return false;
}

// search through the SSA graph to find a version with GetDefBy() == DefBYy_stmt
// visited is for avoiding processing a node more than once
RegMeExpr *RegMeExpr::FindDefByStmt(std::set<RegMeExpr*> &visited) {
  if (visited.find(this) != visited.end()) {
    return nullptr;
  }
  visited.insert(this);
  if (GetDefBy() == kDefByStmt) {
    return this;
  }
  if (GetDefBy() == kDefByPhi) {
    MeRegPhiNode &phiReg = GetDefPhi();
    for (RegMeExpr *phiOpnd : phiReg.GetOpnds()) {
      RegMeExpr *res = phiOpnd->FindDefByStmt(visited);
      if (res != nullptr) {
        return res;
      }
    }
  }
  return nullptr;
}

MeExpr &MeExpr::GetAddrExprBase() {
  switch (meOp) {
    case kMeOpAddrof:
    case kMeOpVar:
      return *this;
    case kMeOpOp:
      if (op == OP_add || op == OP_sub) {
        auto *opMeExpr = static_cast<OpMeExpr*>(this);
        return opMeExpr->GetOpnd(0)->GetAddrExprBase();
      }
      return *this;
    case kMeOpNary:
      if (op == OP_array) {
        auto *naryExpr = static_cast<NaryMeExpr*>(this);
        ASSERT(naryExpr->GetOpnd(0) != nullptr, "");
        return naryExpr->GetOpnd(0)->GetAddrExprBase();
      }
      return *this;
    case kMeOpReg: {
      auto *baseVar = static_cast<RegMeExpr*>(this);
      std::set<RegMeExpr*> visited;
      baseVar = baseVar->FindDefByStmt(visited);
      if (baseVar != nullptr && baseVar->GetDefBy() == kDefByStmt) {
        MeStmt *baseDefStmt = baseVar->GetDefStmt();
        auto *regAssign = static_cast<RegassignMeStmt*>(baseDefStmt);
        MeExpr *rhs = regAssign->GetRHS();
        // Following we only consider array, add and sub
        // Prevent the following situation for reg %1
        // regAssign ref %1 (gcmallocjarray ref ...)
        if (rhs->op == OP_array || rhs->op == OP_add || rhs->op == OP_sub) {
          return rhs->GetAddrExprBase();
        }
      }
      return *this;
    }
    default:
      return *this;
  }
}

bool NaryMeExpr::IsUseSameSymbol(const MeExpr &expr) const {
  if (expr.GetMeOp() != kMeOpNary) {
    return false;
  }
  auto &naryMeExpr = static_cast<const NaryMeExpr&>(expr);
  if (expr.GetOp() != GetOp() || naryMeExpr.GetIntrinsic() != intrinsic || naryMeExpr.tyIdx != tyIdx) {
    return false;
  }
  if (opnds.size() != naryMeExpr.GetOpnds().size()) {
    return false;
  }
  for (size_t i = 0; i < opnds.size(); ++i) {
    if (!opnds[i]->IsUseSameSymbol(*naryMeExpr.GetOpnd(i))) {
      return false;
    }
  }
  return true;
}

bool MeExpr::IsAllOpndsIdentical(const MeExpr &meExpr) const {
  for (uint8 i = 0; i < GetNumOpnds(); ++i) {
    if (GetOpnd(i)->GetExprID() != meExpr.GetOpnd(i)->GetExprID()) {
      return false;
    }
  }
  return true;
}

bool OpMeExpr::IsAllOpndsIdentical(const OpMeExpr &meExpr) const {
  // add/mul/or/xor/and/etc..., need not to consider opnd order
  if (IsCommutative(meExpr.GetOp())) {
    ASSERT(meExpr.GetNumOpnds() == 2, "not supported opcode: %d", meExpr.GetOp());
    return (meExpr.GetOpnd(0) == this->GetOpnd(0) && meExpr.GetOpnd(1) == this->GetOpnd(1)) ||
           (meExpr.GetOpnd(1) == this->GetOpnd(0) && meExpr.GetOpnd(0) == this->GetOpnd(1));
  }
  return MeExpr::IsAllOpndsIdentical(meExpr);
}

bool OpMeExpr::IsCompareIdentical(const OpMeExpr &meExpr) const {
  // x > y <==> y < x; x <= y <==> y >= x;
  switch (meExpr.GetOp()) {
    case OP_ge:
      if (this->GetOp() != OP_le) {
        return false;
      }
      break;
    case OP_le:
      if (this->GetOp() != OP_ge) {
        return false;
      }
      break;
    case OP_gt:
      if (this->GetOp() != OP_lt) {
        return false;
      }
      break;
    case OP_lt:
      if (this->GetOp() != OP_gt) {
        return false;
      }
      break;
    default:
      return false;
  }

  return (meExpr.GetOpnd(1) == this->GetOpnd(0)) && (meExpr.GetOpnd(0) == this->GetOpnd(1));
}

bool OpMeExpr::IsIdentical(const OpMeExpr &meExpr) const {
  if (meExpr.GetOp() != GetOp()) {
    return false;
  }
  if (meExpr.GetPrimType() != GetPrimType() || meExpr.opndType != opndType || meExpr.bitsOffset != bitsOffset ||
      meExpr.bitsSize != bitsSize || meExpr.tyIdx != tyIdx || meExpr.fieldID != fieldID) {
    return false;
  }
  if (IsAllOpndsIdentical(meExpr)) {
    return true;
  }

  if (IsCompareIdentical(meExpr)) {
    return true;
  }

  return false;
}

bool NaryMeExpr::IsIdentical(NaryMeExpr &meExpr) const {
  if (meExpr.GetOp() != GetOp() || meExpr.tyIdx != tyIdx || meExpr.GetIntrinsic() != intrinsic ||
      meExpr.boundCheck != boundCheck) {
    return false;
  }
  if (meExpr.GetNumOpnds() != GetNumOpnds()) {
    return false;
  }
  return IsAllOpndsIdentical(meExpr);
}

bool IvarMeExpr::IsUseSameSymbol(const MeExpr &expr) const {
  if (expr.GetExprID() == GetExprID()) {
    return true;
  }
  if (expr.GetMeOp() != kMeOpIvar) {
    return false;
  }
  auto &ivarMeExpr = static_cast<const IvarMeExpr&>(expr);
  CHECK_FATAL(base != nullptr, "base is null");
  if (base->IsUseSameSymbol(*ivarMeExpr.base) && fieldID == ivarMeExpr.fieldID) {
    return true;
  }
  return false;
}

bool IvarMeExpr::IsVolatile() const {
  auto *type = static_cast<MIRPtrType*>(GlobalTables::GetTypeTable().GetTypeFromTyIdx(tyIdx));
  MIRType *pointedType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(type->GetPointedTyIdx());
  if (fieldID == 0) {
    return pointedType->HasVolatileField();
  }
  return static_cast<MIRStructType*>(pointedType)->IsFieldVolatile(fieldID);
}

bool IvarMeExpr::IsFinal() {
  auto *type = static_cast<MIRPtrType*>(GlobalTables::GetTypeTable().GetTypeFromTyIdx(tyIdx));
  MIRType *pointedType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(type->GetPointedTyIdx());
  if (fieldID == 0) {
    return false;
  }
  return static_cast<MIRStructType*>(pointedType)->IsFieldFinal(fieldID);
}


// check paragma
//   pragma 0 var %keySet <$Ljava_2Flang_2Fannotation_2FRCWeakRef_3B>

bool IvarMeExpr::IsRCWeak() const {
  auto *type = static_cast<MIRPtrType*>(GlobalTables::GetTypeTable().GetTypeFromTyIdx(tyIdx));
  MIRType *pointedType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(type->GetPointedTyIdx());
  if (pointedType->GetKind() == kTypeClass) {
    auto *classType = static_cast<MIRClassType*>(pointedType);
    return classType->IsFieldRCWeak(fieldID);
  }
  return false;
}

// If self is the first use of the same ivar coming from an iassign
// (argument expr), then update its mu: expr->mu = this->mu.
bool IvarMeExpr::IsIdentical(IvarMeExpr &expr) const {
  CHECK_FATAL(expr.base != nullptr, "null ptr check");
  if (base->GetExprID() != expr.base->GetExprID() || fieldID != expr.fieldID || tyIdx != expr.tyIdx) {
    return false;
  }

  // check the two mu being identical
  if (mu != expr.mu) {
    if (mu != nullptr && expr.mu != nullptr && mu->GetDefBy() == kDefByChi && expr.mu->GetDefBy() == kDefByChi) {
      if (mu->GetDefChi().GetBase() != nullptr && mu->GetDefChi().GetBase() == expr.mu->GetDefChi().GetBase()) {
        return true;
      }
    }

    if (mu != nullptr && expr.mu == nullptr && expr.GetDefStmt() != nullptr) {
      IassignMeStmt *iass = expr.GetDefStmt();
      if (iass->GetOp() != OP_iassign) {
        // this can happen due to use of placement new
        return false;
      }
      for (auto xit = iass->GetChiList()->begin(); xit != iass->GetChiList()->end(); ++xit) {
        ChiMeNode *chi = xit->second;
        if (chi->GetLHS()->GetExprID() == mu->GetExprID()) {
          expr.SetMuVal(mu);
          return true;
        }
      }
    }
    return false;
  }
  return true;
}

MeExpr *IvarMeExpr::GetIdenticalExpr(MeExpr &expr) const {
  auto *ivarExpr = static_cast<IvarMeExpr*>(&expr);

  while (ivarExpr != nullptr) {
    if (ivarExpr->GetMeOp() == kMeOpIvar && IsIdentical(*ivarExpr)) {
      return ivarExpr;
    }
    ivarExpr = static_cast<IvarMeExpr*>(ivarExpr->GetNext());
  }
  return nullptr;
}

BB *VarMeExpr::GetDefByBBMeStmt(const Dominance &dominance, MeStmtPtr &defMeStmt) const {
  switch (defBy) {
    case kDefByNo:
      return &dominance.GetCommonEntryBB();
    case kDefByStmt:
      defMeStmt = def.defStmt;
      return defMeStmt->GetBB();
    case kDefByMustDef:
      defMeStmt = def.defMustDef->GetBase();
      return defMeStmt->GetBB();
    case kDefByChi:
      defMeStmt = def.defChi->GetBase();
      return defMeStmt->GetBB();
    case kDefByPhi:
      return def.defPhi->GetDefBB();
    default:
      return nullptr;
  }
}

bool VarMeExpr::IsUseSameSymbol(const MeExpr &expr) const {
  if (expr.GetMeOp() != kMeOpVar) {
    return false;
  }
  auto &varMeExpr = static_cast<const VarMeExpr&>(expr);
  return ostIdx == varMeExpr.ostIdx;
}

bool VarMeExpr::IsPureLocal(const SSATab &tab, const MIRFunction &irFunc) const {
  const MIRSymbol *st = tab.GetMIRSymbolFromID(ostIdx);
  return st->IsLocal() && !irFunc.IsAFormal(st);
}

bool VarMeExpr::IsZeroVersion(const SSATab &ssatab) const {
  ASSERT(vstIdx != 0, "VarMeExpr::IsZeroVersion: cannot determine because vstIdx is 0");
  const OriginalSt *ost = ssatab.GetOriginalStFromID(ostIdx);
  return ost->GetZeroVersionIndex() == vstIdx;
}

bool RegMeExpr::IsUseSameSymbol(const MeExpr &expr) const {
  if (expr.GetMeOp() != kMeOpReg) {
    return false;
  }
  auto &regMeExpr = static_cast<const RegMeExpr&>(expr);
  return ostIdx == regMeExpr.ostIdx;
}

BB *RegMeExpr::DefByBB() {
  switch (defBy) {
    case kDefByNo:
      return nullptr;
    case kDefByStmt:
      ASSERT(def.defStmt, "VarMeExpr::DefByBB: defStmt cannot be nullptr");
      return def.defStmt->GetBB();
    case kDefByPhi:
      ASSERT(def.defPhi, "VarMeExpr::DefByBB: defPhi cannot be nullptr");
      return def.defPhi->GetDefBB();
    case kDefByMustDef:
      ASSERT(def.defMustDef, "VarMeExpr::DefByBB: defMustDef cannot be nullptr");
      ASSERT(def.defMustDef->GetBase(), "VarMeExpr::DefByBB: defMustDef->base cannot be nullptr");
      return def.defMustDef->GetBase()->GetBB();
    default:
      ASSERT(false, "reg define unknown");
      return nullptr;
  }
}

bool AddrofMeExpr::IsUseSameSymbol(const MeExpr &expr) const {
  if (expr.GetMeOp() != kMeOpAddrof) {
    return false;
  }
  const auto &varMeExpr = static_cast<const AddrofMeExpr&>(expr);
  return ostIdx == varMeExpr.ostIdx;
}

bool OpMeExpr::IsUseSameSymbol(const MeExpr &expr) const {
  if (expr.GetOp() != GetOp()) {
    return false;
  }
  if (expr.GetMeOp() != kMeOpOp) {
    return false;
  }
  auto &opMeExpr = static_cast<const OpMeExpr&>(expr);
  for (uint32 i = 0; i < kOperandNumTernary; ++i) {
    if (opnds[i]) {
      if (!opMeExpr.opnds[i]) {
        return false;
      }
      if (!opnds[i]->IsUseSameSymbol(*opMeExpr.opnds[i])) {
        return false;
      }
    } else {
      if (opMeExpr.opnds[i]) {
        return false;
      }
    }
  }
  return true;
}

MeExpr *OpMeExpr::GetIdenticalExpr(MeExpr &expr) const {
  if (!kOpcodeInfo.NotPure(GetOp())) {
    auto *opExpr = static_cast<OpMeExpr*>(&expr);

    while (opExpr != nullptr) {
      if (IsIdentical(*opExpr)) {
        return opExpr;
      }
      opExpr = static_cast<OpMeExpr*>(opExpr->GetNext());
    }
  }

  return nullptr;
}

// first, make sure it's int const and return true if the int const great or eq 0
bool ConstMeExpr::GeZero() const {
  return (GetIntValue() >= 0);
}

bool ConstMeExpr::GtZero() const {
  CHECK_FATAL(constVal != nullptr, "constVal is null");
  if (constVal->GetKind() != kConstInt) {
    return false;
  }
  return (safe_cast<MIRIntConst>(constVal)->GetValue() > 0);
}

bool ConstMeExpr::IsZero() const {
  return (GetIntValue() == 0);
}

bool ConstMeExpr::IsOne() const {
  CHECK_FATAL(constVal != nullptr, "constVal is null");
  if (constVal->GetKind() != kConstInt) {
    return false;
  }
  return (safe_cast<MIRIntConst>(constVal)->GetValue() == 1);
}

int64 ConstMeExpr::GetIntValue() const {
  CHECK_FATAL(constVal != nullptr, "constVal is null");
  CHECK_FATAL(constVal->GetKind() == kConstInt, "expect int const");
  return safe_cast<MIRIntConst>(constVal)->GetValue();
}

MeExpr *ConstMeExpr::GetIdenticalExpr(MeExpr &expr) const {
  auto *constExpr = static_cast<ConstMeExpr*>(&expr);

  while (constExpr != nullptr) {
    if (constExpr->GetMeOp() == kMeOpConst && constExpr->GetPrimType() == GetPrimType() &&
        *constExpr->GetConstVal() == *constVal) {
      return constExpr;
    }
    constExpr = static_cast<ConstMeExpr*>(constExpr->GetNext());
  }

  return nullptr;
}

MeExpr *ConststrMeExpr::GetIdenticalExpr(MeExpr &expr) const {
  auto *constStrExpr = static_cast<ConststrMeExpr*>(&expr);

  while (constStrExpr != nullptr) {
    if (constStrExpr->GetMeOp() == kMeOpConststr && constStrExpr->GetStrIdx() == strIdx) {
      return constStrExpr;
    }
    constStrExpr = static_cast<ConststrMeExpr*>(constStrExpr->GetNext());
  }

  return nullptr;
}

MeExpr *Conststr16MeExpr::GetIdenticalExpr(MeExpr &expr) const {
  auto *constStr16Expr = static_cast<Conststr16MeExpr*>(&expr);

  while (constStr16Expr != nullptr) {
    if (constStr16Expr->GetMeOp() == kMeOpConststr16 && constStr16Expr->GetStrIdx() == strIdx) {
      return constStr16Expr;
    }
    constStr16Expr = static_cast<Conststr16MeExpr*>(constStr16Expr->GetNext());
  }

  return nullptr;
}

MeExpr *SizeoftypeMeExpr::GetIdenticalExpr(MeExpr &expr) const {
  auto *sizeoftypeExpr = static_cast<SizeoftypeMeExpr*>(&expr);

  while (sizeoftypeExpr != nullptr) {
    if (sizeoftypeExpr->GetMeOp() == kMeOpSizeoftype && sizeoftypeExpr->GetTyIdx() == tyIdx) {
      return sizeoftypeExpr;
    }
    sizeoftypeExpr = static_cast<SizeoftypeMeExpr*>(sizeoftypeExpr->GetNext());
  }

  return nullptr;
}

MeExpr *FieldsDistMeExpr::GetIdenticalExpr(MeExpr &expr) const {
  auto *fieldsDistExpr = static_cast<FieldsDistMeExpr*>(&expr);

  while (fieldsDistExpr != nullptr) {
    if (fieldsDistExpr->GetMeOp() == kMeOpFieldsDist && fieldsDistExpr->GetTyIdx() == GetTyIdx() &&
        fieldsDistExpr->GetFieldID1() == fieldID1 && fieldsDistExpr->GetFieldID2() == fieldID2) {
        return fieldsDistExpr;
    }
    fieldsDistExpr = static_cast<FieldsDistMeExpr*>(fieldsDistExpr->GetNext());
  }

  return nullptr;
}

MeExpr *AddrofMeExpr::GetIdenticalExpr(MeExpr &expr) const {
  auto *addrofExpr = static_cast<AddrofMeExpr*>(&expr);

  while (addrofExpr != nullptr) {
    if (addrofExpr->GetMeOp() == kMeOpAddrof && addrofExpr->GetOstIdx() == GetOstIdx() &&
        addrofExpr->GetFieldID() == fieldID) {
      return addrofExpr;
    }
    addrofExpr = static_cast<AddrofMeExpr*>(addrofExpr->GetNext());
  }

  return nullptr;
}

MeExpr *NaryMeExpr::GetIdenticalExpr(MeExpr &expr) const {
  auto *naryExpr = static_cast<NaryMeExpr*>(&expr);

  while (naryExpr != nullptr) {
    bool isPureIntrinsic =
        naryExpr->GetOp() == OP_intrinsicop && IntrinDesc::intrinTable[naryExpr->GetIntrinsic()].IsPure();
    if ((naryExpr->GetOp() == OP_array || isPureIntrinsic) && IsIdentical(*naryExpr)) {
      return naryExpr;
    }
    naryExpr = static_cast<NaryMeExpr*>(naryExpr->GetNext());
  }

  return nullptr;
}

MeExpr *AddroffuncMeExpr::GetIdenticalExpr(MeExpr &expr) const {
  auto *addroffuncExpr = static_cast<AddroffuncMeExpr*>(&expr);

  while (addroffuncExpr != nullptr) {
    if (addroffuncExpr->GetMeOp() == kMeOpAddroffunc && addroffuncExpr->GetPuIdx() == puIdx) {
      return addroffuncExpr;
    }
    addroffuncExpr = static_cast<AddroffuncMeExpr*>(addroffuncExpr->GetNext());
  }

  return nullptr;
}

void MeVarPhiNode::Dump(const IRMap *irMap) const {
  if (isPiAdded) {
    LogInfo::MapleLogger() << "PI_ADD VAR:";
  }
  LogInfo::MapleLogger() << "VAR:";
  CHECK_FATAL(lhs != nullptr, "lsh is null");
  irMap->GetSSATab().GetOriginalStFromID(lhs->GetOStIdx())->Dump();
  LogInfo::MapleLogger() << " mx" << lhs->GetExprID();
  LogInfo::MapleLogger() << " = MEPHI{";
  for (size_t i = 0; i < opnds.size(); ++i) {
    LogInfo::MapleLogger() << "mx" << opnds[i]->GetExprID();
    if (i != opnds.size() - 1) {
      LogInfo::MapleLogger() << ",";
    }
  }
  LogInfo::MapleLogger() << "}";
  if (!isLive) {
    LogInfo::MapleLogger() << " dead";
  }
  LogInfo::MapleLogger() << '\n';
}

void MeRegPhiNode::Dump(const IRMap *irMap) const {
  CHECK_FATAL(lhs != nullptr, "lhs is null");
  LogInfo::MapleLogger() << "REGVAR: " << lhs->GetRegIdx();
  LogInfo::MapleLogger() << "(%"
                         << irMap->GetMIRModule().CurFunction()
                                                 ->GetPregTab()
                                                 ->PregFromPregIdx(static_cast<PregIdx>(lhs->GetRegIdx()))
                                                 ->GetPregNo()
                         << ")";
  LogInfo::MapleLogger() << " mx" << lhs->GetExprID();
  LogInfo::MapleLogger() << " = MEPHI{";
  for (size_t i = 0; i < opnds.size(); ++i) {
    LogInfo::MapleLogger() << "mx" << opnds[i]->GetExprID();
    if (i != opnds.size() - 1) {
      LogInfo::MapleLogger() << ",";
    }
  }
  LogInfo::MapleLogger() << "}" << '\n';
}

void VarMeExpr::Dump(const IRMap *irMap, int32) const {
  CHECK_NULL_FATAL(irMap);
  LogInfo::MapleLogger() << "VAR ";
  irMap->GetSSATab().GetOriginalStFromID(ostIdx)->Dump();
  LogInfo::MapleLogger() << " (field)" << fieldID;
  LogInfo::MapleLogger() << " mx" << GetExprID();
  if (IsZeroVersion(irMap->GetSSATab())) {
    LogInfo::MapleLogger() << "<Z>";
  }
}

void RegMeExpr::Dump(const IRMap *irMap, int32) const {
  CHECK_NULL_FATAL(irMap);
  LogInfo::MapleLogger() << "REGINDX:" << regIdx;
  LogInfo::MapleLogger()
      << " %"
      << irMap->GetMIRModule().CurFunction()->GetPregTab()->PregFromPregIdx(static_cast<PregIdx>(regIdx))->GetPregNo();
  LogInfo::MapleLogger() << " mx" << GetExprID();
}

void AddroffuncMeExpr::Dump(const IRMap*, int32) const {
  LogInfo::MapleLogger() << "ADDROFFUNC:";
  LogInfo::MapleLogger() << GlobalTables::GetFunctionTable().GetFunctionFromPuidx(puIdx)->GetName();
  LogInfo::MapleLogger() << " mx" << GetExprID();
}

void GcmallocMeExpr::Dump(const IRMap*, int32) const {
  LogInfo::MapleLogger() << kOpcodeInfo.GetTableItemAt(GetOp()).name << " " << GetPrimTypeName(GetPrimType());
  LogInfo::MapleLogger() << " ";
  GlobalTables::GetTypeTable().GetTypeFromTyIdx(tyIdx)->Dump(0);
  LogInfo::MapleLogger() << " mx" << GetExprID();
  LogInfo::MapleLogger() << " ";
}

void ConstMeExpr::Dump(const IRMap*, int32) const {
  LogInfo::MapleLogger() << "CONST";
  LogInfo::MapleLogger() << " ";
  CHECK_FATAL(constVal != nullptr, "constVal is null");
  constVal->Dump();
  LogInfo::MapleLogger() << " mx" << GetExprID();
}

void ConststrMeExpr::Dump(const IRMap*, int32) const {
  LogInfo::MapleLogger() << "CONSTSTR";
  LogInfo::MapleLogger() << " ";
  LogInfo::MapleLogger() << strIdx;
  LogInfo::MapleLogger() << " mx" << GetExprID();
}

void Conststr16MeExpr::Dump(const IRMap*, int32) const {
  LogInfo::MapleLogger() << "CONSTSTR16";
  LogInfo::MapleLogger() << " ";
  LogInfo::MapleLogger() << strIdx;
  LogInfo::MapleLogger() << " mx" << GetExprID();
}

void SizeoftypeMeExpr::Dump(const IRMap*, int32) const {
  LogInfo::MapleLogger() << kOpcodeInfo.GetTableItemAt(GetOp()).name << " " << GetPrimTypeName(GetPrimType());
  LogInfo::MapleLogger() << " TYIDX:" << tyIdx;
  MIRType *mirType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(tyIdx);
  mirType->Dump(0);
  LogInfo::MapleLogger() << " mx" << GetExprID();
}

void FieldsDistMeExpr::Dump(const IRMap*, int32) const {
  LogInfo::MapleLogger() << kOpcodeInfo.GetTableItemAt(GetOp()).name << " " << GetPrimTypeName(GetPrimType());
  LogInfo::MapleLogger() << " TYIDX:" << tyIdx;
  MIRType *mirType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(tyIdx);
  mirType->Dump(0);
  LogInfo::MapleLogger() << " (field)" << fieldID1;
  LogInfo::MapleLogger() << " (field)" << fieldID2;
  LogInfo::MapleLogger() << " mx" << GetExprID();
}

void AddrofMeExpr::Dump(const IRMap *irMap, int32) const {
  CHECK_NULL_FATAL(irMap);
  LogInfo::MapleLogger() << "ADDROF:";
  irMap->GetSSATab().GetOriginalStFromID(ostIdx)->Dump();
  LogInfo::MapleLogger() << " (field)" << fieldID;
  LogInfo::MapleLogger() << " mx" << GetExprID();
}

void OpMeExpr::Dump(const IRMap *irMap, int32 indent) const {
  LogInfo::MapleLogger() << "OP " << kOpcodeInfo.GetTableItemAt(GetOp()).name;
  LogInfo::MapleLogger() << " mx" << GetExprID();
  LogInfo::MapleLogger() << '\n';
  ASSERT(opnds[0] != nullptr, "OpMeExpr::Dump: cannot have 0 operand");
  PrintIndentation(indent + 1);
  LogInfo::MapleLogger() << "opnd[0] = ";
  opnds[0]->Dump(irMap, indent + 1);
  if (opnds[1]) {
    LogInfo::MapleLogger() << '\n';
  } else {
    return;
  }
  PrintIndentation(indent + 1);
  LogInfo::MapleLogger() << "opnd[1] = ";
  opnds[1]->Dump(irMap, indent + 1);
  if (opnds[2]) {
    LogInfo::MapleLogger() << '\n';
  } else {
    return;
  }
  PrintIndentation(indent + 1);
  LogInfo::MapleLogger() << "opnd[2] = ";
  opnds[2]->Dump(irMap, indent + 1);
}

void IvarMeExpr::Dump(const IRMap *irMap, int32 indent) const {
  LogInfo::MapleLogger() << "IVAR mx" << GetExprID();
  LogInfo::MapleLogger() << " " << GetPrimTypeName(GetPrimType());
  LogInfo::MapleLogger() << " TYIDX:" << tyIdx;
  MIRType *mirType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(tyIdx);
  mirType->Dump(0);
  LogInfo::MapleLogger() << " (field)" << fieldID << '\n';
  PrintIndentation(indent + 1);
  LogInfo::MapleLogger() << "base = ";
  CHECK_FATAL(base != nullptr, "base is null");
  base->Dump(irMap, indent + 1);
  LogInfo::MapleLogger() << '\n';
  PrintIndentation(indent + 1);
  LogInfo::MapleLogger() << "- MU: {";
  if (mu != nullptr) {
    mu->Dump(irMap);
  }
  LogInfo::MapleLogger() << "}";
}

void NaryMeExpr::Dump(const IRMap *irMap, int32 indent) const {
  ASSERT(static_cast<size_t>(GetNumOpnds()) == opnds.size(), "array size error");
  if (GetOp() == OP_array) {
    LogInfo::MapleLogger() << "ARRAY ";
  } else if (GetOp() == OP_intrinsicop) {
    LogInfo::MapleLogger() << GetIntrinsicName(intrinsic);
  } else {
    ASSERT(GetOp() == OP_intrinsicopwithtype, "NaryMeExpr has bad GetOp()code");
    LogInfo::MapleLogger() << "INTRINOPWTY[" << intrinsic << "]";
  }
  LogInfo::MapleLogger() << " mx" << GetExprID() << '\n';
  for (int32 i = 0; i < GetNumOpnds(); ++i) {
    PrintIndentation(indent + 1);
    LogInfo::MapleLogger() << "opnd[" << i << "] = ";
    opnds[i]->Dump(irMap, indent + 1);
    if (i != GetNumOpnds() - 1) {
      LogInfo::MapleLogger() << '\n';
    }
  }
}

MeExpr *DassignMeStmt::GetLHSRef(SSATab &ssaTab, bool excludeLocalRefVar) {
  VarMeExpr *lhsOpnd = GetVarLHS();
  if (lhsOpnd->GetPrimType() != PTY_ref) {
    return nullptr;
  }
  const OriginalSt *ost = ssaTab.GetOriginalStFromID(lhsOpnd->GetOStIdx());
  if (ost->IsIgnoreRC()) {
    return nullptr;
  }
  if (excludeLocalRefVar && ost->IsLocal()) {
    return nullptr;
  }
  return lhsOpnd;
}

MeExpr *MaydassignMeStmt::GetLHSRef(SSATab &ssaTab, bool excludeLocalRefVar) {
  VarMeExpr *lhs = GetVarLHS();
  if (lhs->GetPrimType() != PTY_ref) {
    return nullptr;
  }
  const OriginalSt *ost = ssaTab.GetOriginalStFromID(lhs->GetOStIdx());
  if (ost->IsIgnoreRC()) {
    return nullptr;
  }
  if (excludeLocalRefVar && ost->IsLocal()) {
    return nullptr;
  }
  return lhs;
}

MeExpr *IassignMeStmt::GetLHSRef(SSATab&, bool) {
  CHECK_FATAL(lhsVar != nullptr, "lhsVar is null");
  MIRType *baseType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(lhsVar->GetTyIdx());
  ASSERT(baseType != nullptr, "null ptr check");
  auto *pType = static_cast<MIRPtrType*>(baseType)->GetPointedType();
  if (!pType->IsStructType()) {
    if (pType->GetKind() == kTypePointer) {
      if (lhsVar->GetFieldID() == 0) {
        if (static_cast<MIRPtrType*>(pType)->GetPrimType() != PTY_ref) {
          return nullptr;
        }
      } else {
        auto *ppType = static_cast<MIRPtrType*>(pType)->GetPointedType();
        auto fTyIdx = static_cast<MIRStructType*>(ppType)->GetFieldTyIdx(lhsVar->GetFieldID());
        if (GlobalTables::GetTypeTable().GetTypeFromTyIdx(fTyIdx)->GetPrimType() != PTY_ref) {
          return nullptr;
        }
      }
    } else if (pType->GetKind() == kTypeJArray) {
      auto *ppType = static_cast<MIRPtrType*>(pType)->GetPointedType();
      if (static_cast<MIRPtrType*>(ppType)->GetPrimType() != PTY_ref) {
        return nullptr;
      }
    } else {
      return nullptr;
    }
  } else {
    auto *structType = static_cast<MIRStructType*>(pType);
    if (lhsVar->GetFieldID() == 0) {
      return nullptr;  // struct assign is not ref
    }
    if (structType->GetFieldType(lhsVar->GetFieldID())->GetPrimType() != PTY_ref) {
      return nullptr;
    }
  }
  return lhsVar;
}

VarMeExpr *AssignedPart::GetAssignedPartLHSRef(SSATab &ssaTab, bool excludeLocalRefVar) {
  if (mustDefList.empty()) {
    return nullptr;
  }
  MeExpr *assignedLHS = mustDefList.front().GetLHS();
  if (assignedLHS->GetMeOp() != kMeOpVar) {
    return nullptr;
  }
  auto *theLHS = static_cast<VarMeExpr*>(assignedLHS);
  if (theLHS->GetPrimType() != PTY_ref) {
    return nullptr;
  }
  const OriginalSt *ost = ssaTab.GetOriginalStFromID(theLHS->GetOStIdx());
  if (ost->IsIgnoreRC()) {
    return nullptr;
  }
  if (excludeLocalRefVar && ost->IsLocal()) {
    return nullptr;
  }
  return theLHS;
}

// default Dump
void MeStmt::Dump(const IRMap *irMap) const {
  if (op == OP_comment) {
    return;
  }
  CHECK_NULL_FATAL(irMap);
  irMap->GetMIRModule().GetOut() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(op).name << '\n';
}

// get the real next mestmt that is not a comment
MeStmt *MeStmt::GetNextMeStmt() const {
  MeStmt *nextMeStmt = next;
  while (nextMeStmt != nullptr && nextMeStmt->op == OP_comment) {
    nextMeStmt = nextMeStmt->next;
  }
  return nextMeStmt;
}

void PiassignMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR PI|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << " ";
  CHECK_FATAL(lhs != nullptr, "lhs is null");
  lhs->Dump(irMap);
  LogInfo::MapleLogger() << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << "rhs = ";
  rhs->Dump(irMap, kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << '\n';
}

void DassignMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << " ";
  CHECK_FATAL(lhs != nullptr, "lhs is null");
  lhs->Dump(irMap);
  LogInfo::MapleLogger() << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << "rhs = ";
  CHECK_FATAL(rhs != nullptr, "rhs is null");
  rhs->Dump(irMap, kDefaultPrintIndentNum);
  if (needIncref) {
    LogInfo::MapleLogger() << " [RC+]";
  }
  if (needDecref) {
    LogInfo::MapleLogger() << " [RC-]";
  }
  LogInfo::MapleLogger() << '\n';
  DumpChiList(irMap, chiList);
}

void RegassignMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << " ";
  CHECK_FATAL(lhs != nullptr, "lhs is null");
  lhs->Dump(irMap);
  LogInfo::MapleLogger() << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << "rhs = ";
  CHECK_FATAL(rhs != nullptr, "rhs is null");
  rhs->Dump(irMap, kDefaultPrintIndentNum);
  if (needIncref) {
    LogInfo::MapleLogger() << " [RC+]";
  }
  LogInfo::MapleLogger() << '\n';
}

void MaydassignMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << "rhs = ";
  CHECK_FATAL(rhs != nullptr, "rhs is null");
  rhs->Dump(irMap, kDefaultPrintIndentNum);
  if (needIncref) {
    LogInfo::MapleLogger() << " [RC+]";
  }
  if (needDecref) {
    LogInfo::MapleLogger() << " [RC-]";
  }
  LogInfo::MapleLogger() << '\n';
  DumpChiList(irMap, chiList);
}

void ChiMeNode::Dump(const IRMap *irMap) const {
  CHECK_NULL_FATAL(irMap);
  auto *meLHS = static_cast<VarMeExpr*>(lhs);
  auto *meRHS = static_cast<VarMeExpr*>(rhs);
  CHECK_FATAL(meLHS != nullptr, "Node doesn't have lhs?");
  CHECK_FATAL(meRHS != nullptr, "Node doesn't have rhs?");
  if (!DumpOptions::GetSimpleDump()) {
    LogInfo::MapleLogger() << "VAR:";
    irMap->GetSSATab().GetOriginalStFromID(meLHS->GetOStIdx())->Dump();
  }
  LogInfo::MapleLogger() << " mx" << meLHS->GetExprID() << " = CHI{";
  LogInfo::MapleLogger() << "mx" << meRHS->GetExprID() << "}";
}

void DumpMuList(const IRMap *irMap, const MapleMap<OStIdx, VarMeExpr*> &muList) {
  if (muList.empty()) {
    return;
  }
  int count = 0;
  LogInfo::MapleLogger() << "---- MULIST: { ";
  for (auto it = muList.begin();;) {
    if (!DumpOptions::GetSimpleDump()) {
      (*it).second->Dump(irMap);
    } else {
      LogInfo::MapleLogger() << "mx" << (*it).second->GetExprID();
    }
    ++it;
    if (it == muList.end()) {
      break;
    } else {
      LogInfo::MapleLogger() << ", ";
    }
    if (DumpOptions::GetDumpVsyNum() > 0 && ++count >= DumpOptions::GetDumpVsyNum()) {
      LogInfo::MapleLogger() << " ... ";
      break;
    }
  }
  LogInfo::MapleLogger() << " }\n";
}

void DumpChiList(const IRMap *irMap, const MapleMap<OStIdx, ChiMeNode*> &chiList) {
  if (chiList.empty()) {
    return;
  }
  int count = 0;
  LogInfo::MapleLogger() << "---- CHILIST: { ";
  for (auto it = chiList.begin();;) {
    it->second->Dump(irMap);
    ++it;
    if (it == chiList.end()) {
      break;
    } else {
      LogInfo::MapleLogger() << ", ";
    }
    if (DumpOptions::GetDumpVsyNum() > 0 && count++ >= DumpOptions::GetDumpVsyNum()) {
      LogInfo::MapleLogger() << " ... ";
      break;
    }
  }
  LogInfo::MapleLogger() << " }\n";
}

void IassignMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << "lhs = ";
  CHECK_FATAL(lhsVar != nullptr, "lhsVar is null");
  lhsVar->Dump(irMap, kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << "rhs = ";
  CHECK_FATAL(rhs != nullptr, "rhs is null");
  rhs->Dump(irMap, kDefaultPrintIndentNum);
  if (needIncref) {
    LogInfo::MapleLogger() << " [RC+]";
  }
  if (needDecref) {
    LogInfo::MapleLogger() << " [RC-]";
  }
  LogInfo::MapleLogger() << '\n';
  DumpChiList(irMap, chiList);
}

void NaryMeStmt::DumpOpnds(const IRMap *irMap) const {
  for (size_t i = 0; i < opnds.size(); ++i) {
    PrintIndentation(kDefaultPrintIndentNum);
    LogInfo::MapleLogger() << "opnd[" << i << "] = ";
    opnds[i]->Dump(irMap, kDefaultPrintIndentNum);
    LogInfo::MapleLogger() << '\n';
  }
}

void NaryMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << " ";
  DumpOpnds(irMap);
}

void AssignedPart::DumpAssignedPart(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "    assignedpart: {";
  for (auto it = mustDefList.begin(); it != mustDefList.end(); ++it) {
    const MeExpr *lhsVar = (*it).GetLHS();
    lhsVar->Dump(irMap);
  }
  if (needIncref) {
    LogInfo::MapleLogger() << " [RC+]";
  }
  if (needDecref) {
    LogInfo::MapleLogger() << " [RC-]";
  }
  LogInfo::MapleLogger() << "}\n";
}

void CallMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << " ";
  if (tyIdx != 0u) {
    LogInfo::MapleLogger() << " TYIDX:" << tyIdx;
    MIRType *mirType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(tyIdx);
    mirType->Dump(0);
  }
  // target function name
  MIRFunction *func = GlobalTables::GetFunctionTable().GetFunctionFromPuidx(puIdx);
  if (irMap->GetMIRModule().IsJavaModule()) {
    LogInfo::MapleLogger() << NameMangler::DecodeName(func->GetName()) << '\n';
  } else {
    LogInfo::MapleLogger() << func->GetName() << '\n';
  }
  DumpOpnds(irMap);
  DumpMuList(irMap, muList);
  DumpChiList(irMap, chiList);
  DumpAssignedPart(irMap);
}

void IcallMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << " ";
  LogInfo::MapleLogger() << " TYIDX:" << retTyIdx;
  DumpOpnds(irMap);
  DumpMuList(irMap, muList);
  DumpChiList(irMap, chiList);
  DumpAssignedPart(irMap);
}

void IntrinsiccallMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << " ";
  LogInfo::MapleLogger() << "TYIDX:" << tyIdx;
  MIRType *mirType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(tyIdx);
  if (mirType != nullptr) {
    mirType->Dump(0);
  }
  LogInfo::MapleLogger() << GetIntrinsicName(intrinsic) << '\n';
  DumpOpnds(irMap);
  DumpMuList(irMap, muList);
  DumpChiList(irMap, chiList);
  DumpAssignedPart(irMap);
}

void RetMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << '\n';
  DumpOpnds(irMap);
  DumpMuList(irMap, muList);
}

void CondGotoMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << "cond: ";
  auto mirFunc = static_cast<const MeIRMap*>(irMap)->GetFunc().GetMirFunc();
  LogInfo::MapleLogger() << " @" << mirFunc->GetLabelName((LabelIdx)offset);
  GetOpnd()->Dump(irMap, kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << '\n';
}

void UnaryMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << " unaryopnd: ";
  CHECK_FATAL(opnd != nullptr, "opnd is null");
  opnd->Dump(irMap, kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << '\n';
}

void SwitchMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << "switchOpnd: ";
  GetOpnd()->Dump(irMap, kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << '\n';
}

void GosubMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << '\n';
  DumpMuList(irMap, *GetMuList());
  LogInfo::MapleLogger() << '\n';
}

void ThrowMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << "throwopnd: ";
  CHECK_FATAL(opnd != nullptr, "opnd is null");
  opnd->Dump(irMap, kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << '\n';
  DumpMuList(irMap, *GetMuList());
}

void SyncMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << '\n';
  DumpOpnds(irMap);
  DumpMuList(irMap, muList);
  DumpChiList(irMap, chiList);
}

bool MeStmt::IsTheSameWorkcand(const MeStmt &mestmt) const {
  if (op != mestmt.op) {
    return false;
  }
  if (op == OP_dassign) {
    if (this->GetVarLHS()->GetOStIdx() != mestmt.GetVarLHS()->GetOStIdx()) {
      return false;
    }
  } else if (op == OP_intrinsiccallwithtype) {
    if (static_cast<const IntrinsiccallMeStmt*>(this)->GetTyIdx() !=
        static_cast<const IntrinsiccallMeStmt &>(mestmt).GetTyIdx()) {
      return false;
    }
    if (static_cast<const IntrinsiccallMeStmt*>(this)->GetIntrinsic() !=
        static_cast<const IntrinsiccallMeStmt &>(mestmt).GetIntrinsic()) {
      return false;
    }
  } else if (op == OP_callassigned) {
    auto *thisCass = static_cast<const CallMeStmt*>(this);
    auto &cass = static_cast<const CallMeStmt &>(mestmt);
    if (thisCass->GetPUIdx() != cass.GetPUIdx()) {
      return false;
    }
    if (thisCass->MustDefListSize() != cass.MustDefListSize()) {
      return false;
    }
    if (thisCass->MustDefListSize() > 0) {
      auto *thisVarMeExpr = static_cast<const VarMeExpr*>(thisCass->GetAssignedLHS());
      auto *varMeExpr = static_cast<const VarMeExpr*>(cass.GetAssignedLHS());
      if (thisVarMeExpr->GetOStIdx() != varMeExpr->GetOStIdx()) {
        return false;
      }
    }
  }
  // check the operands
  for (size_t i = 0; i < NumMeStmtOpnds(); ++i) {
    ASSERT(GetOpnd(i) != nullptr, "null ptr check");
    if (!GetOpnd(i)->IsUseSameSymbol(*mestmt.GetOpnd(i))) {
      return false;
    }
  }
  return true;
}

void AssertMeStmt::Dump(const IRMap *irMap) const {
  LogInfo::MapleLogger() << "||MEIR|| " << kOpcodeInfo.GetTableItemAt(GetOp()).name << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << "opnd[0] = ";
  opnds[0]->Dump(irMap, kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << '\n';
  PrintIndentation(kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << "opnd[1] = ";
  opnds[1]->Dump(irMap, kDefaultPrintIndentNum);
  LogInfo::MapleLogger() << '\n';
}

BB *VarMeExpr::DefByBB() {
  switch (defBy) {
    case kDefByNo:
      return nullptr;
    case kDefByStmt:
      ASSERT(def.defStmt, "VarMeExpr::DefByBB: defStmt cannot be nullptr");
      return def.defStmt->GetBB();
    case kDefByPhi:
      ASSERT(def.defPhi, "VarMeExpr::DefByBB: defPhi cannot be nullptr");
      return def.defPhi->GetDefBB();
    case kDefByChi: {
      ASSERT(def.defChi, "VarMeExpr::DefByBB: defChi cannot be nullptr");
      ASSERT(def.defChi->GetBase(), "VarMeExpr::DefByBB: defChi->base cannot be nullptr");
      return def.defChi->GetBase()->GetBB();
    }
    case kDefByMustDef: {
      ASSERT(def.defMustDef, "VarMeExpr::DefByBB: defMustDef cannot be nullptr");
      ASSERT(def.defMustDef->GetBase(), "VarMeExpr::DefByBB: defMustDef->base cannot be nullptr");
      return def.defMustDef->GetBase()->GetBB();
    }
    default:
      ASSERT(false, "var define unknown");
      return nullptr;
  }
}

bool VarMeExpr::IsVolatile(const SSATab &ssatab) const {
  const OriginalSt *ost = ssatab.GetOriginalStFromID(ostIdx);
  if (!ost->IsSymbolOst()) {
    return false;
  }
  const MIRSymbol *sym = ost->GetMIRSymbol();
  if (sym->IsVolatile()) {
    return true;
  }
  MIRType *type = GlobalTables::GetTypeTable().GetTypeFromTyIdx(sym->GetTyIdx());
  if (fieldID == 0) {
    return (type->HasVolatileField());
  }
  auto *structType = static_cast<MIRStructType*>(type);
  return structType->IsFieldVolatile(fieldID);
}

bool VarMeExpr::PointsToStringLiteral() {
  VarMeExpr &var = ResolveVarMeValue();
  if (var.GetDefBy() == kDefByMustDef) {
    MeStmt *baseStmt = var.GetDefMustDef().GetBase();
    if (baseStmt->GetOp() == OP_callassigned) {
      auto *call = static_cast<CallMeStmt*>(baseStmt);
      MIRFunction &callFunc = call->GetTargetFunction();
      if (callFunc.GetName() == "MCC_GetOrInsertLiteral") {
        return true;
      }
    }
  }
  return false;
}

MeExpr *MeExpr::FindSymAppearance(OStIdx oidx) {
  if (meOp == kMeOpVar) {
    if (static_cast<VarMeExpr*>(this)->GetOStIdx() == oidx) {
      return this;
    }
    return nullptr;
  }
  for (uint8 i = 0; i < GetNumOpnds(); ++i) {
    MeExpr *retx = GetOpnd(i)->FindSymAppearance(oidx);
    if (retx != nullptr) {
      return retx;
    }
  }
  return nullptr;
}

bool MeExpr::SymAppears(OStIdx oidx) {
  return FindSymAppearance(oidx) != nullptr;
}

bool MeExpr::HasIvar() const {
  if (meOp == kMeOpIvar) {
    return true;
  }
  for (uint8 i = 0; i < GetNumOpnds(); ++i) {
    ASSERT(GetOpnd(i) != nullptr, "null ptr check");
    if (GetOpnd(i)->HasIvar()) {
      return true;
    }
  }
  return false;
}


// check if MeExpr can be a pointer to something that requires incref for its
// assigned target
bool MeExpr::PointsToSomethingThatNeedsIncRef() {
  if (op == OP_retype) {
    return true;
  }
  if (meOp == kMeOpIvar) {
    return true;
  }
  if (meOp == kMeOpVar) {
    auto *var = static_cast<VarMeExpr*>(this);
    if (var->PointsToStringLiteral()) {
      return false;
    }
    return true;
  }
  if (meOp == kMeOpReg) {
    auto *r = static_cast<RegMeExpr*>(this);
    return r->GetRegIdx() != -kSregThrownval;
  }
  return false;
}

MapleMap<OStIdx, ChiMeNode*> *GenericGetChiListFromVarMeExprInner(VarMeExpr &expr,
                                                                  std::unordered_set<VarMeExpr*> &visited) {
  if (expr.GetDefBy() == kDefByNo || visited.find(&expr) != visited.end()) {
    return nullptr;
  }
  visited.insert(&expr);
  if (expr.GetDefBy() == kDefByPhi) {
    MeVarPhiNode &phiMe = expr.GetDefPhi();
    MapleVector<VarMeExpr*> &phiOpnds = phiMe.GetOpnds();
    for (auto it = phiOpnds.begin(); it != phiOpnds.end(); ++it) {
      VarMeExpr *meExpr = *it;
      MapleMap<OStIdx, ChiMeNode*> *chiList = GenericGetChiListFromVarMeExprInner(*meExpr, visited);
      if (chiList != nullptr) {
        return chiList;
      }
    }
    return nullptr;
  }
  if (expr.GetDefBy() == kDefByChi) {
    return expr.GetDefChi().GetBase()->GetChiList();
  }
  return nullptr;
}

MapleMap<OStIdx, ChiMeNode*> *GenericGetChiListFromVarMeExpr(VarMeExpr &expr) {
  std::unordered_set<VarMeExpr*> visited;
  return GenericGetChiListFromVarMeExprInner(expr, visited);
}

void CallMeStmt::SetCallReturn(MeExpr &curexpr) {
  MustDefMeNode &mustDefMeNode = GetMustDefList()->front();
  mustDefMeNode.UpdateLHS(curexpr);
}

bool DumpOptions::simpleDump = false;
int DumpOptions::dumpVsymNum = 0;
}  // namespace maple
