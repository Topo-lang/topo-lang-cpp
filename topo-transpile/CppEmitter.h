#ifndef TOPO_TRANSPILE_CPPEMITTER_H
#define TOPO_TRANSPILE_CPPEMITTER_H

#include "topo/Transpile/Emitter.h"
#include "topo/Sema/TypeBinder.h"

namespace topo::transpile {

class CppEmitter : public Emitter {
public:
    explicit CppEmitter(TypeBinder binder = TypeBinder::createDefault(HostLanguage::Cpp));
    EmitResult emit(const TranspileModule& module) override;

private:
    TypeBinder binder_;

    std::string emitType(const TypeNode& type);
    std::string emitExpr(const Expr& expr);
    std::string emitStmt(const Stmt& stmt, int indent);
    std::string emitFunction(const TranspileFunction& func);
    std::string emitStruct(const TranspileType& type);
    std::string emitOwnership(const TypeNode& type);
};

} // namespace topo::transpile

#endif // TOPO_TRANSPILE_CPPEMITTER_H
