#pragma once

#include "mystral/js/engine.h"
#include "mystral/js/module_resolver.h"
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mystral {
namespace js {

class ModuleSystem {
public:
    ModuleSystem(Engine* engine, const std::string& rootDir);

    bool loadEntry(const std::string& entryPath);
    JSValueHandle require(const std::string& specifier, const std::string& referrer);

    bool resolveForImport(const std::string& specifier,
                          const std::string& referrer,
                          ResolvedModule& out,
                          std::string& error);

    bool getEsmSource(const ResolvedModule& resolved,
                      const std::string& referrer,
                      std::string& outSource,
                      std::string& outFilename,
                      std::string& error);

    const std::unordered_set<std::string>& loadedPaths() const;
    void clearCaches();
    ModuleResolver& resolver();

private:
    Engine* engine_ = nullptr;
    ModuleResolver resolver_;
    std::unordered_map<std::string, JSValueHandle> cjsCache_;
    std::unordered_set<std::string> loading_;
    std::unordered_set<std::string> loadedPaths_;

    bool loadEsmEntry(const ResolvedModule& resolved, const std::string& source);
    JSValueHandle requireResolved(const ResolvedModule& resolved);
    JSValueHandle createRequireFunction(const std::string& referrer);
    std::string makeCjsWrapper(const std::string& code, const std::string& filename) const;
    std::string makeJsonWrapper(const std::string& jsonText) const;
    std::string makeEsmWrapper(const std::string& resolvedPath) const;
    std::string makeAbsoluteSpecifier(const ResolvedModule& resolved) const;
    std::string escapeJsString(const std::string& input) const;
    bool maybeTranspileTypeScript(const ResolvedModule& resolved,
                                  std::string& source,
                                  std::string& error);
    JSValueHandle executeCjsModule(const ResolvedModule& resolved,
                                   const std::string& source,
                                   bool sourceIsJson);
};

ModuleSystem* getModuleSystem();
void setModuleSystem(ModuleSystem* system);

}  // namespace js
}  // namespace mystral
