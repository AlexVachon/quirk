#include "ApexModule.hpp"

class StdFileModule : public ApexModule {
   public:
    void registerStructs(LLVMContext& ctx,
                         std::map<std::string, StructType*>& structTypes,
                         StructGen* structGen) override {
        // 1. File Struct
        if (structTypes.find("File") == structTypes.end()) {
            std::vector<Type*> elements = {
                Type::getInt8PtrTy(ctx),  // handle (FILE*)
                Type::getInt32Ty(ctx)     // is_open
            };
            structTypes["File"] = StructType::create(ctx, elements, "File");
        }
        structGen->registerStructLayout("File", {"handle", "is_open"});

        // 2. FileIterator Struct
        if (structTypes.find("FileIterator") == structTypes.end()) {
            // ✅ FIXED: Check that String is registered, error if not
            if (structTypes.find("String") == structTypes.end()) {
                std::cerr
                    << "ERROR: String type not registered! "
                    << "StdStringModule must be registered BEFORE StdFileModule"
                    << std::endl;
                exit(1);
            }

            std::vector<Type*> elements = {
                PointerType::getUnqual(structTypes["File"]),  // file_ref: File*
                PointerType::getUnqual(
                    structTypes["String"])  // current_line: String*
            };
            structTypes["FileIterator"] =
                StructType::create(ctx, elements, "FileIterator");
        }
        structGen->registerStructLayout("FileIterator",
                                        {"file_ref", "current_line"});

        // 3. StringIterator Struct (Optional - for completeness)
        if (structTypes.find("StringIterator") == structTypes.end()) {
            if (structTypes.find("String") != structTypes.end()) {
                std::vector<Type*> elements = {
                    PointerType::getUnqual(
                        structTypes["String"]),  // str_ref: String*
                    Type::getInt32Ty(ctx)        // idx: int
                };
                structTypes["StringIterator"] =
                    StructType::create(ctx, elements, "StringIterator");
                structGen->registerStructLayout("StringIterator",
                                                {"str_ref", "idx"});
            }
        }
    }
};