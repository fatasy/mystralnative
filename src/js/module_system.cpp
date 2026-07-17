#include "mystral/js/module_system.h"

#include "mystral/js/ts_transpiler.h"

#include <cctype>
#include <iostream>
#include <regex>
#include <sstream>

namespace mystral {
namespace js {

namespace {

thread_local ModuleSystem* g_moduleSystem = nullptr;

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool isTypeScriptPath(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = path.substr(dot);
    return ext == ".ts" || ext == ".tsx" || ext == ".mts" || ext == ".cts";
}

std::string trimLeft(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        start++;
    }
    return value.substr(start);
}

std::vector<std::string> splitCommaSeparated(const std::string& value) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    for (char c : value) {
        if (c == '{' || c == '[' || c == '(') {
            depth++;
        } else if (c == '}' || c == ']' || c == ')') {
            depth--;
        }
        if (c == ',' && depth == 0) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::string trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        start++;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        end--;
    }
    return value.substr(start, end - start);
}

}  // namespace

ModuleSystem* getModuleSystem() {
    return g_moduleSystem;
}

void setModuleSystem(ModuleSystem* system) {
    g_moduleSystem = system;
}

ModuleSystem::ModuleSystem(Engine* engine, const std::string& rootDir)
    : engine_(engine)
    , resolver_(rootDir) {}

bool ModuleSystem::loadEntry(const std::string& entryPath) {
    if (!engine_) {
        std::cerr << "[Modules] No JS engine available" << std::endl;
        return false;
    }

    std::string entrySpec = entryPath;
    bool hasFileScheme = startsWith(entryPath, "file://");
    std::string normalized = resolver_.normalizeSpecifier(entrySpec);
    bool isWindowsAbs = normalized.size() > 2 &&
                        std::isalpha(static_cast<unsigned char>(normalized[0])) &&
                        normalized[1] == ':';
    if (!startsWith(normalized, "/") &&
        !startsWith(normalized, "./") &&
        !startsWith(normalized, "../") &&
        !hasFileScheme &&
        !isWindowsAbs) {
        entrySpec = "./" + entrySpec;
    }

    ResolvedModule resolved;
    std::string error;
    if (!resolver_.resolve(entrySpec, "", ResolveMode::Require, resolved, error)) {
        std::cerr << "[Modules] Failed to resolve entry: " << error << std::endl;
        return false;
    }

    loadedPaths_.insert(resolved.resolved.path);

    std::string source;
    if (!resolver_.readFile(resolved.resolved, source, error)) {
        std::cerr << "[Modules] Failed to read entry: " << error << std::endl;
        return false;
    }

    if (!maybeTranspileTypeScript(resolved, source, error)) {
        std::cerr << "[Modules] Failed to transpile TypeScript: " << error << std::endl;
        return false;
    }

    if (resolved.format == ModuleFormat::ESM) {
        // QuickJS uses the CJS transpilation path for entry ESM.
        // V8 has native ESM support via its module API (handled by loadEsmEntry).
        if (engine_->getType() == EngineType::QuickJS) {
            std::string cjs = transpileEsmToCjs(source);
            ResolvedModule cjsModule = resolved;
            cjsModule.format = ModuleFormat::CJS;
            JSValueHandle result = executeCjsModule(cjsModule, cjs, false);
            return result.ptr != nullptr;
        }
        return loadEsmEntry(resolved, source);
    }

    bool isJson = resolved.format == ModuleFormat::JSON;
    JSValueHandle result = executeCjsModule(resolved, source, isJson);
    return result.ptr != nullptr;
}

bool ModuleSystem::loadEsmEntry(const ResolvedModule& resolved, const std::string& source) {
    if (!engine_) {
        return false;
    }
    return engine_->eval(source.c_str(), resolved.resolved.path.c_str());
}

JSValueHandle ModuleSystem::require(const std::string& specifier, const std::string& referrer) {
    if (!engine_) {
        return {};
    }

    ResolvedModule resolved;
    std::string error;
    if (!resolver_.resolve(specifier, referrer, ResolveMode::Require, resolved, error)) {
        std::cerr << "[Modules] require() failed: " << error << std::endl;
        engine_->throwException(error.c_str());
        return engine_->newUndefined();
    }

    if (resolved.format == ModuleFormat::ESM) {
        // QuickJS uses the CJS transpilation path for required ESM.
        if (engine_->getType() == EngineType::QuickJS) {
            std::string source;
            if (!resolver_.readFile(resolved.resolved, source, error)) {
                std::cerr << "[Modules] Failed to read module: " << error << std::endl;
                engine_->throwException(error.c_str());
                return engine_->newUndefined();
            }
            if (!maybeTranspileTypeScript(resolved, source, error)) {
                std::cerr << "[Modules] Failed to transpile TypeScript: " << error << std::endl;
                engine_->throwException(error.c_str());
                return engine_->newUndefined();
            }
            std::string cjs = transpileEsmToCjs(source);
            ResolvedModule cjsModule = resolved;
            cjsModule.format = ModuleFormat::CJS;
            loadedPaths_.insert(resolved.resolved.path);
            return executeCjsModule(cjsModule, cjs, false);
        }
        std::string message = "Cannot require ES module: " + resolved.resolved.path;
        std::cerr << "[Modules] " << message << std::endl;
        engine_->throwException(message.c_str());
        return engine_->newUndefined();
    }

    loadedPaths_.insert(resolved.resolved.path);
    return requireResolved(resolved);
}

JSValueHandle ModuleSystem::requireResolved(const ResolvedModule& resolved) {
    std::string error;
    std::string source;
    if (!resolver_.readFile(resolved.resolved, source, error)) {
        std::cerr << "[Modules] Failed to read module: " << error << std::endl;
        engine_->throwException(error.c_str());
        return engine_->newUndefined();
    }

    if (!maybeTranspileTypeScript(resolved, source, error)) {
        std::cerr << "[Modules] Failed to transpile TypeScript: " << error << std::endl;
        engine_->throwException(error.c_str());
        return engine_->newUndefined();
    }

    return executeCjsModule(resolved, source, resolved.format == ModuleFormat::JSON);
}

JSValueHandle ModuleSystem::executeCjsModule(const ResolvedModule& resolved,
                                             const std::string& source,
                                             bool sourceIsJson) {
    auto cacheIt = cjsCache_.find(resolved.resolved.path);
    if (cacheIt != cjsCache_.end()) {
        return cacheIt->second;
    }

    if (loading_.count(resolved.resolved.path) > 0) {
        auto loadingIt = cjsCache_.find(resolved.resolved.path);
        if (loadingIt != cjsCache_.end()) {
            return loadingIt->second;
        }
        return engine_->newUndefined();
    }

    loading_.insert(resolved.resolved.path);

    std::string code = source;
    if (sourceIsJson) {
        code = makeJsonWrapper(source);
    }

    JSValueHandle exportsObj = engine_->newObject();
    JSValueHandle moduleObj = engine_->newObject();
    engine_->setProperty(moduleObj, "exports", exportsObj);

    engine_->protect(exportsObj);
    cjsCache_[resolved.resolved.path] = exportsObj;

    JSValueHandle requireFn = createRequireFunction(resolved.resolved.path);
    engine_->protect(requireFn);

    std::string wrapped = makeCjsWrapper(code, resolved.resolved.path);
    JSValueHandle wrapperFn = engine_->evalScriptWithResult(wrapped.c_str(), resolved.resolved.path.c_str());
    if (!wrapperFn.ptr) {
        std::cerr << "[Modules] Failed to compile module: " << resolved.resolved.path << std::endl;
        engine_->unprotect(requireFn);
        engine_->unprotect(exportsObj);
        engine_->unprotect(moduleObj);
        cjsCache_.erase(resolved.resolved.path);
        loading_.erase(resolved.resolved.path);
        return engine_->newUndefined();
    }

    JSValueHandle filenameVal = engine_->newString(resolved.resolved.path.c_str());
    std::string dir = resolver_.dirname(resolved.resolved.path);
    JSValueHandle dirnameVal = engine_->newString(dir.c_str());

    JSValueHandle callResult = engine_->call(wrapperFn, engine_->newUndefined(),
        {exportsObj, requireFn, moduleObj, filenameVal, dirnameVal});
    if (!callResult.ptr) {
        std::cerr << "[Modules] Error while executing module: " << resolved.resolved.path << std::endl;
    }

    JSValueHandle moduleExports = engine_->getProperty(moduleObj, "exports");
    if (moduleExports.ptr) {
        auto cached = cjsCache_.find(resolved.resolved.path);
        if (cached != cjsCache_.end()) {
            engine_->unprotect(cached->second);
        }
        engine_->protect(moduleExports);
        cjsCache_[resolved.resolved.path] = moduleExports;
    }

    if (callResult.ptr) {
        engine_->unprotect(callResult);
    }
    if (wrapperFn.ptr) {
        engine_->unprotect(wrapperFn);
    }
    if (filenameVal.ptr) {
        engine_->unprotect(filenameVal);
    }
    if (dirnameVal.ptr) {
        engine_->unprotect(dirnameVal);
    }
    if (moduleObj.ptr) {
        engine_->unprotect(moduleObj);
    }
    engine_->unprotect(requireFn);
    loading_.erase(resolved.resolved.path);
    return moduleExports.ptr ? moduleExports : exportsObj;
}

bool ModuleSystem::resolveForImport(const std::string& specifier,
                                    const std::string& referrer,
                                    ResolvedModule& out,
                                    std::string& error) {
    return resolver_.resolve(specifier, referrer, ResolveMode::Import, out, error);
}

bool ModuleSystem::getEsmSource(const ResolvedModule& resolved,
                                const std::string& referrer,
                                std::string& outSource,
                                std::string& outFilename,
                                std::string& error) {
    error.clear();
    outSource.clear();
    (void)referrer;

    if (resolved.format == ModuleFormat::ESM) {
        if (!resolver_.readFile(resolved.resolved, outSource, error)) {
            return false;
        }
        if (!maybeTranspileTypeScript(resolved, outSource, error)) {
            return false;
        }
        outFilename = resolved.resolved.path;
        loadedPaths_.insert(resolved.resolved.path);
        return true;
    }

    std::string specifier = makeAbsoluteSpecifier(resolved);
    outSource = makeEsmWrapper(specifier);
    outFilename = resolved.resolved.path;
    loadedPaths_.insert(resolved.resolved.path);
    return true;
}

JSValueHandle ModuleSystem::createRequireFunction(const std::string& referrer) {
    return engine_->newFunction("require",
        [this, referrer](void* ctx, const std::vector<JSValueHandle>& args) {
            (void)ctx;
            if (args.empty()) {
                return engine_->newUndefined();
            }
            std::string spec = engine_->toString(args[0]);
            return require(spec, referrer);
        });
}

std::string ModuleSystem::makeCjsWrapper(const std::string& code, const std::string& filename) const {
    std::string wrapped;
    wrapped.reserve(code.size() + 128);
    wrapped += "(function(exports, require, module, __filename, __dirname) {\n";
    wrapped += "'use strict';\n";
    wrapped += code;
    if (!code.empty() && code.back() != '\n') {
        wrapped += "\n";
    }
    wrapped += "})\n";
    return wrapped;
}

std::string ModuleSystem::makeJsonWrapper(const std::string& jsonText) const {
    std::string wrapped = "module.exports = ";
    wrapped += jsonText;
    wrapped += ";\n";
    return wrapped;
}

std::string ModuleSystem::makeEsmWrapper(const std::string& resolvedPath) const {
    std::string escaped = escapeJsString(resolvedPath);
    std::string source = "const __cjs = __mystralRequire(\"" + escaped + "\", \"\");\n";
    source += "export default __cjs;\n";
    return source;
}

std::string ModuleSystem::makeAbsoluteSpecifier(const ResolvedModule& resolved) const {
    if (resolver_.usingBundle()) {
        if (startsWith(resolved.resolved.path, "/")) {
            return resolved.resolved.path;
        }
        return "/" + resolved.resolved.path;
    }
    return resolved.resolved.path;
}

std::string ModuleSystem::escapeJsString(const std::string& input) const {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string ModuleSystem::transpileEsmToCjs(const std::string& source) const {
    std::istringstream input(source);
    std::ostringstream output;
    std::vector<std::string> deferredExports;
    bool usesExports = false;

    std::string line;
    while (std::getline(input, line)) {
        std::string trimmed = trimLeft(line);

        std::smatch match;
        std::regex importDefault(R"(^import\s+([A-Za-z_$][\w$]*)\s+from\s+['"]([^'"]+)['"]\s*;?\s*$)");
        std::regex importAll(R"(^import\s+\*\s+as\s+([A-Za-z_$][\w$]*)\s+from\s+['"]([^'"]+)['"]\s*;?\s*$)");
        std::regex importNamed(R"(^import\s+\{([^}]+)\}\s+from\s+['"]([^'"]+)['"]\s*;?\s*$)");
        std::regex importMixed(R"(^import\s+([A-Za-z_$][\w$]*)\s*,\s*\{([^}]+)\}\s+from\s+['"]([^'"]+)['"]\s*;?\s*$)");
        std::regex importMixedAll(R"(^import\s+([A-Za-z_$][\w$]*)\s*,\s*\*\s+as\s+([A-Za-z_$][\w$]*)\s+from\s+['"]([^'"]+)['"]\s*;?\s*$)");
        std::regex importSideEffect(R"(^import\s+['"]([^'"]+)['"]\s*;?\s*$)");

        if (std::regex_match(trimmed, match, importMixed)) {
            std::string defaultName = match[1];
            std::string namedList = trim(match[2]);
            std::string spec = match[3];
            output << "const __mod = require(\"" << spec << "\");\n";
            output << "const " << defaultName << " = (__mod && __mod.__esModule) ? __mod.default : __mod;\n";
            output << "const { " << namedList << " } = __mod;\n";
            continue;
        }
        if (std::regex_match(trimmed, match, importMixedAll)) {
            std::string defaultName = match[1];
            std::string nsName = match[2];
            std::string spec = match[3];
            output << "const __mod = require(\"" << spec << "\");\n";
            output << "const " << defaultName << " = (__mod && __mod.__esModule) ? __mod.default : __mod;\n";
            output << "const " << nsName << " = __mod;\n";
            continue;
        }
        if (std::regex_match(trimmed, match, importDefault)) {
            std::string defaultName = match[1];
            std::string spec = match[2];
            output << "const __mod = require(\"" << spec << "\");\n";
            output << "const " << defaultName << " = (__mod && __mod.__esModule) ? __mod.default : __mod;\n";
            continue;
        }
        if (std::regex_match(trimmed, match, importAll)) {
            std::string nsName = match[1];
            std::string spec = match[2];
            output << "const " << nsName << " = require(\"" << spec << "\");\n";
            continue;
        }
        if (std::regex_match(trimmed, match, importNamed)) {
            std::string namedList = trim(match[1]);
            std::string spec = match[2];
            output << "const { " << namedList << " } = require(\"" << spec << "\");\n";
            continue;
        }
        if (std::regex_match(trimmed, match, importSideEffect)) {
            std::string spec = match[1];
            output << "require(\"" << spec << "\");\n";
            continue;
        }

        std::regex exportDefaultFunc(R"(^export\s+default\s+function\s+([A-Za-z_$][\w$]*)\s*\()");
        std::regex exportDefaultClass(R"(^export\s+default\s+class\s+([A-Za-z_$][\w$]*)\s*)");
        std::regex exportDefault(R"(^export\s+default\s+(.+);?\s*$)");
        std::regex exportNamedDecl(R"(^export\s+(const|let|var|function|class)\s+([A-Za-z_$][\w$]*)\s*)");
        std::regex exportNamedList(R"(^export\s+\{([^}]+)\}\s*;?\s*$)");
        std::regex exportNamedFrom(R"(^export\s+\{([^}]+)\}\s+from\s+['"]([^'"]+)['"]\s*;?\s*$)");
        std::regex exportAllFrom(R"(^export\s+\*\s+from\s+['"]([^'"]+)['"]\s*;?\s*$)");

        if (std::regex_search(trimmed, match, exportDefaultFunc)) {
            usesExports = true;
            line = std::regex_replace(line, std::regex("^\\s*export\\s+default\\s+"), "");
            output << line << "\n";
            deferredExports.push_back("exports.default = " + match[1].str() + ";");
            continue;
        }
        if (std::regex_search(trimmed, match, exportDefaultClass)) {
            usesExports = true;
            line = std::regex_replace(line, std::regex("^\\s*export\\s+default\\s+"), "");
            output << line << "\n";
            deferredExports.push_back("exports.default = " + match[1].str() + ";");
            continue;
        }
        if (std::regex_match(trimmed, match, exportNamedFrom)) {
            usesExports = true;
            std::string list = trim(match[1]);
            std::string spec = match[2];
            output << "const __mod = require(\"" << spec << "\");\n";
            for (const auto& part : splitCommaSeparated(list)) {
                std::string item = trim(part);
                size_t asPos = item.find(" as ");
                if (asPos != std::string::npos) {
                    std::string left = trim(item.substr(0, asPos));
                    std::string right = trim(item.substr(asPos + 4));
                    if (left == "default") {
                        output << "exports." << right << " = (__mod && __mod.__esModule) ? __mod.default : __mod;\n";
                    } else {
                        output << "exports." << right << " = __mod." << left << ";\n";
                    }
                } else {
                    output << "exports." << item << " = __mod." << item << ";\n";
                }
            }
            continue;
        }
        if (std::regex_match(trimmed, match, exportAllFrom)) {
            usesExports = true;
            std::string spec = match[1];
            output << "Object.assign(exports, require(\"" << spec << "\"));\n";
            continue;
        }
        if (std::regex_match(trimmed, match, exportNamedList)) {
            usesExports = true;
            std::string list = trim(match[1]);
            for (const auto& part : splitCommaSeparated(list)) {
                std::string item = trim(part);
                size_t asPos = item.find(" as ");
                if (asPos != std::string::npos) {
                    std::string left = trim(item.substr(0, asPos));
                    std::string right = trim(item.substr(asPos + 4));
                    output << "exports." << right << " = " << left << ";\n";
                } else {
                    output << "exports." << item << " = " << item << ";\n";
                }
            }
            continue;
        }
        if (std::regex_search(trimmed, match, exportNamedDecl)) {
            usesExports = true;
            line = std::regex_replace(line, std::regex("^\\s*export\\s+"), "");
            output << line << "\n";
            deferredExports.push_back("exports." + match[2].str() + " = " + match[2].str() + ";");
            continue;
        }
        if (std::regex_match(trimmed, match, exportDefault)) {
            usesExports = true;
            output << "exports.default = " << match[1] << ";\n";
            continue;
        }

        output << line << "\n";
    }

    for (const auto& assignment : deferredExports) {
        output << assignment << "\n";
    }

    if (usesExports) {
        std::ostringstream finalOut;
        finalOut << "exports.__esModule = true;\n";
        finalOut << output.str();
        return finalOut.str();
    }

    return output.str();
}

bool ModuleSystem::maybeTranspileTypeScript(const ResolvedModule& resolved,
                                            std::string& source,
                                            std::string& error) {
    if (!isTypeScriptPath(resolved.resolved.path)) {
        return true;
    }

    if (!isTypeScriptTranspilerAvailable()) {
        error = "TypeScript support is not enabled (missing SWC)";
        return false;
    }

    std::string outJs;
    if (!transpileTypeScript(source, resolved.resolved.path, outJs, error)) {
        if (error.empty()) {
            error = "TypeScript transpile failed";
        }
        return false;
    }

    source.swap(outJs);
    return true;
}

const std::unordered_set<std::string>& ModuleSystem::loadedPaths() const {
    return loadedPaths_;
}

void ModuleSystem::clearCaches() {
    for (auto& entry : cjsCache_) {
        engine_->unprotect(entry.second);
    }
    cjsCache_.clear();
    loading_.clear();
    loadedPaths_.clear();
}

ModuleResolver& ModuleSystem::resolver() {
    return resolver_;
}

}  // namespace js
}  // namespace mystral
