#include "QuirkModule.hpp"

class StdMathModule : public QuirkModule {
   public:
    void registerStructs(LLVMContext& ctx,
                         std::map<std::string, StructType*>& structTypes,
                         StructGen* structGen) override {
        if (structTypes.find("Vector2") == structTypes.end()) {
            std::vector<Type*> elements = {Type::getDoubleTy(ctx),
                                           Type::getDoubleTy(ctx)};
            structTypes["Vector2"] =
                StructType::create(ctx, elements, "Vector2");
        }
        structGen->registerStructLayout("Vector2", {"x", "y"});

        if (structTypes.find("Vector3") == structTypes.end()) {
            std::vector<Type*> elements = {
                Type::getDoubleTy(ctx),  // x
                Type::getDoubleTy(ctx),  // y
                Type::getDoubleTy(ctx)   // z
            };
            structTypes["Vector3"] =
                StructType::create(ctx, elements, "Vector3");
        }
        structGen->registerStructLayout("Vector3", {"x", "y", "z"});
    }
};