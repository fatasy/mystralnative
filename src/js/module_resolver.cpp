#include "mystral/js/module_resolver.h"

#include "mystral/vfs/embedded_bundle.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mystral {
namespace js {

namespace fs = std::filesystem;

namespace {

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

}  // namespace

ModuleResolver::ModuleResolver(const std::string& rootDir)
    : rootDir_(rootDir)
    , useBundle_(vfs::hasEmbeddedBundle()) {
    if (rootDir_.empty()) {
        rootDir_ = ".";
    }
}

void ModuleResolver::setRootDir(const std::string& rootDir) {
    rootDir_ = rootDir.empty() ? "." : rootDir;
}

bool ModuleResolver::usingBundle() const {
    return useBundle_;
}

std::string ModuleResolver::normalizeSpecifier(const std::string& specifier) const {
    std::string normalized = specifier;
    if (startsWith(normalized, "file://")) {
        normalized = normalized.substr(7);
    }
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

std::string ModuleResolver::dirname(const std::string& path) const {
    fs::path p(path);
    fs::path parent = p.parent_path();
    if (parent.empty()) {
        return useBundle_ ? std::string() : rootDir_;
    }
    return parent.generic_string();
}

bool ModuleResolver::resolve(const std::string& specifier,
                             const std::string& referrer,
                             ResolveMode mode,
                             ResolvedModule& out,
                             std::string& error) {
    error.clear();

    std::string normalized = normalizeSpecifier(specifier);
    if (normalized.empty()) {
        error = "Empty module specifier";
        return false;
    }

    if (normalized == "mystral/shared" || normalized == "mystral:shared") {
        out.resolved = {"mystral:shared", true};
        out.format = ModuleFormat::ESM;
        return true;
    }
    if (normalized == "mystral/worker-pool" || normalized == "mystral:worker-pool") {
        out.resolved = {"mystral:worker-pool", true};
        out.format = ModuleFormat::ESM;
        return true;
    }

    if (normalized[0] == '#') {
        return resolveImports(normalized, referrer, mode, out, error);
    }

    bool isWindowsAbs = normalized.size() > 2 &&
                        std::isalpha(static_cast<unsigned char>(normalized[0])) &&
                        normalized[1] == ':';
    bool isPathSpec = startsWith(normalized, "/") ||
                      startsWith(normalized, "./") ||
                      startsWith(normalized, "../") ||
                      isWindowsAbs;
    if (!isPathSpec && useBundle_) {
        std::string bundlePath = vfs::normalizeBundlePath(normalized);
        if (!bundlePath.empty() && fileExists(bundlePath)) {
            isPathSpec = true;
            normalized = bundlePath;
        }
    }

    if (isPathSpec) {
        return resolvePath(normalized, referrer, mode, out, error);
    }

    return resolvePackage(normalized, referrer, mode, out, error);
}

bool ModuleResolver::resolveResolvedPath(const std::string& resolvedPath,
                                         ResolvedModule& out,
                                         std::string& error) {
    error.clear();
    std::string normalized = normalizeSpecifier(resolvedPath);
    if (normalized == "mystral:shared") {
        out.resolved = {"mystral:shared", true};
        out.format = ModuleFormat::ESM;
        return true;
    }
    if (normalized == "mystral:worker-pool") {
        out.resolved = {"mystral:worker-pool", true};
        out.format = ModuleFormat::ESM;
        return true;
    }
    if (useBundle_) {
        if (startsWith(normalized, "/")) {
            normalized.erase(0, 1);
        }
        normalized = vfs::normalizeBundlePath(normalized);
        if (normalized.empty()) {
            error = "Invalid bundle module path";
            return false;
        }
        out.resolved = {normalized, true};
    } else {
        fs::path absPath = fs::absolute(normalized).lexically_normal();
        out.resolved = {absPath.generic_string(), false};
    }

    out.format = detectFormatForPath(out.resolved.path);
    return true;
}

bool ModuleResolver::resolvePath(const std::string& pathSpec,
                                 const std::string& referrer,
                                 ResolveMode mode,
                                 ResolvedModule& out,
                                 std::string& error) {
    std::string normalized = normalizeSpecifier(pathSpec);
    std::string baseDir = referrer.empty()
        ? (useBundle_ ? std::string() : rootDir_)
        : dirname(referrer);

    if (useBundle_) {
        std::string rel = normalized;
        if (startsWith(rel, "/")) {
            rel.erase(0, 1);
        } else {
            fs::path combined = fs::path(baseDir) / rel;
            rel = combined.lexically_normal().generic_string();
        }

        rel = vfs::normalizeBundlePath(rel);
        if (rel.empty()) {
            error = "Invalid bundle path";
            return false;
        }

        if (resolveAsFile(rel, mode, out, error)) {
            return true;
        }
        if (resolveAsDirectory(rel, mode, out, error)) {
            return true;
        }
        return false;
    }

    fs::path absPath;
    bool isWindowsAbs = normalized.size() > 2 &&
                        std::isalpha(static_cast<unsigned char>(normalized[0])) &&
                        normalized[1] == ':';
    if (startsWith(normalized, "/") || isWindowsAbs) {
        absPath = fs::path(normalized);
    } else {
        absPath = fs::path(baseDir) / normalized;
    }
    absPath = fs::absolute(absPath).lexically_normal();
    std::string absString = absPath.generic_string();

    if (resolveAsFile(absString, mode, out, error)) {
        return true;
    }
    if (resolveAsDirectory(absString, mode, out, error)) {
        return true;
    }

    return false;
}

bool ModuleResolver::resolveImports(const std::string& specifier,
                                    const std::string& referrer,
                                    ResolveMode mode,
                                    ResolvedModule& out,
                                    std::string& error) {
    std::string baseDir = referrer.empty()
        ? (useBundle_ ? std::string() : rootDir_)
        : dirname(referrer);
    std::string packageRoot;
    if (!findNearestPackage(baseDir, packageRoot)) {
        error = "No package.json found for imports resolution";
        return false;
    }

    PackageInfo pkg;
    if (!loadPackageJson(packageRoot, pkg, error) || !pkg.hasImports) {
        if (error.empty()) {
            error = "No imports defined in package.json";
        }
        return false;
    }

    std::vector<std::string> conditions;
    if (mode == ResolveMode::Import) {
        conditions = {"import", "node", "default"};
    } else {
        conditions = {"require", "node", "default"};
    }

    std::string target;
    if (!resolveExportsTarget(pkg.importsValue, specifier, conditions, target, error)) {
        return false;
    }

    if (startsWith(target, "./") || startsWith(target, "../") || startsWith(target, "/")) {
        std::string combined = (fs::path(packageRoot) / target).generic_string();
        return resolvePath(combined, "", mode, out, error);
    }

    return resolvePackage(target, packageRoot, mode, out, error);
}

bool ModuleResolver::resolvePackage(const std::string& specifier,
                                    const std::string& referrer,
                                    ResolveMode mode,
                                    ResolvedModule& out,
                                    std::string& error) {
    std::string normalized = normalizeSpecifier(specifier);
    std::string packageName;
    std::string subpath = ".";

    if (startsWith(normalized, "@")) {
        size_t slash = normalized.find('/');
        if (slash == std::string::npos) {
            error = "Invalid scoped package specifier";
            return false;
        }
        size_t second = normalized.find('/', slash + 1);
        if (second == std::string::npos) {
            packageName = normalized;
        } else {
            packageName = normalized.substr(0, second);
            subpath = "./" + normalized.substr(second + 1);
        }
    } else {
        size_t slash = normalized.find('/');
        if (slash == std::string::npos) {
            packageName = normalized;
        } else {
            packageName = normalized.substr(0, slash);
            subpath = "./" + normalized.substr(slash + 1);
        }
    }

    std::string baseDir = referrer.empty()
        ? (useBundle_ ? std::string() : rootDir_)
        : dirname(referrer);
    std::string packageRoot;
    if (!findPackageRoot(baseDir, packageName, packageRoot)) {
        error = "Package not found: " + packageName;
        return false;
    }

    PackageInfo pkg;
    std::string pkgError;
    bool hasPackageJson = loadPackageJson(packageRoot, pkg, pkgError);

    if (hasPackageJson && pkg.hasExports) {
        if (resolvePackageExports(pkg, subpath, mode, out, error)) {
            return true;
        }
        return false;
    }

    if (subpath != ".") {
        std::string combined = (fs::path(packageRoot) / subpath.substr(2)).generic_string();
        return resolvePath(combined, "", mode, out, error);
    }

    if (hasPackageJson) {
        if (resolvePackageMain(pkg, mode, out, error)) {
            return true;
        }
    }

    return resolvePath((fs::path(packageRoot) / "index.js").string(), packageRoot, mode, out, error);
}

bool ModuleResolver::resolveAsFile(const std::string& path,
                                   ResolveMode mode,
                                   ResolvedModule& out,
                                   std::string& error) {
    std::string candidate = path;
    if (fileExists(candidate)) {
        out.resolved = {candidate, useBundle_};
        out.format = detectFormatForPath(candidate);
        return true;
    }

    if (mode == ResolveMode::Import) {
        error = "Module not found (import requires extension): " + path;
        return false;
    }

    static const char* extensions[] = {".js", ".json", ".mjs", ".cjs", ".ts", ".tsx", ".mts", ".cts"};
    for (const char* ext : extensions) {
        std::string withExt = candidate + ext;
        if (fileExists(withExt)) {
            out.resolved = {withExt, useBundle_};
            out.format = detectFormatForPath(withExt);
            return true;
        }
    }

    error = "Module not found: " + path;
    return false;
}

bool ModuleResolver::resolveAsDirectory(const std::string& path,
                                        ResolveMode mode,
                                        ResolvedModule& out,
                                        std::string& error) {
    if (!dirExists(path)) {
        error = "Directory not found: " + path;
        return false;
    }

    std::string pkgPath = (fs::path(path) / "package.json").string();
    if (fileExists(pkgPath)) {
        PackageInfo pkg;
        std::string pkgError;
        if (loadPackageJson(path, pkg, pkgError)) {
            if (pkg.hasExports && mode == ResolveMode::Import) {
                if (resolvePackageExports(pkg, ".", mode, out, error)) {
                    return true;
                }
                return false;
            }
            if (!pkg.main.empty()) {
                if (resolvePath((fs::path(path) / pkg.main).generic_string(), "", mode, out, error)) {
                    return true;
                }
            }
        }
    }

    if (mode == ResolveMode::Import) {
        error = "Unsupported directory import: " + path;
        return false;
    }

    std::string indexPath = (fs::path(path) / "index").string();
    if (resolveAsFile(indexPath, mode, out, error)) {
        return true;
    }

    error = "Directory module not found: " + path;
    return false;
}

bool ModuleResolver::resolvePackageMain(const PackageInfo& pkg,
                                        ResolveMode mode,
                                        ResolvedModule& out,
                                        std::string& error) {
    if (!pkg.main.empty()) {
        return resolvePath((fs::path(pkg.rootPath) / pkg.main).generic_string(), "", mode, out, error);
    }
    return resolvePath((fs::path(pkg.rootPath) / "index.js").generic_string(), "", mode, out, error);
}

bool ModuleResolver::resolvePackageExports(const PackageInfo& pkg,
                                           const std::string& subpath,
                                           ResolveMode mode,
                                           ResolvedModule& out,
                                           std::string& error) {
    if (!pkg.hasExports) {
        error = "Package has no exports";
        return false;
    }

    std::vector<std::string> conditions;
    if (mode == ResolveMode::Import) {
        conditions = {"import", "node", "default"};
    } else {
        conditions = {"require", "node", "default"};
    }

    std::string target;
    if (!resolveExportsTarget(pkg.exportsValue, subpath, conditions, target, error)) {
        return false;
    }

    if (!startsWith(target, "./") && !startsWith(target, "../") && !startsWith(target, "/")) {
        error = "Invalid exports target: " + target;
        return false;
    }

    std::string combined = (fs::path(pkg.rootPath) / target).generic_string();
    return resolvePath(combined, "", mode, out, error);
}

bool ModuleResolver::loadPackageJson(const std::string& packageRoot,
                                     PackageInfo& out,
                                     std::string& error) const {
    auto cacheIt = packageCache_.find(packageRoot);
    if (cacheIt != packageCache_.end()) {
        out = cacheIt->second;
        return true;
    }

    std::string pkgPath = (fs::path(packageRoot) / "package.json").string();
    std::string data;
    ResolvedPath resolvedPath;
    resolvedPath.path = pkgPath;
    resolvedPath.isBundle = useBundle_;
    if (!readFile(resolvedPath, data, error)) {
        return false;
    }

    JsonValue root;
    if (!parseJson(data, root, error) || root.type != JsonValue::Type::Object) {
        if (error.empty()) {
            error = "Invalid package.json";
        }
        return false;
    }

    PackageInfo info;
    info.rootPath = packageRoot;

    auto nameIt = root.objectVal.find("name");
    if (nameIt != root.objectVal.end() && nameIt->second.type == JsonValue::Type::String) {
        info.name = nameIt->second.stringVal;
    }
    auto typeIt = root.objectVal.find("type");
    if (typeIt != root.objectVal.end() && typeIt->second.type == JsonValue::Type::String) {
        info.type = typeIt->second.stringVal;
    }
    auto mainIt = root.objectVal.find("main");
    if (mainIt != root.objectVal.end() && mainIt->second.type == JsonValue::Type::String) {
        info.main = mainIt->second.stringVal;
    }
    auto exportsIt = root.objectVal.find("exports");
    if (exportsIt != root.objectVal.end()) {
        info.hasExports = true;
        info.exportsValue = exportsIt->second;
    }
    auto importsIt = root.objectVal.find("imports");
    if (importsIt != root.objectVal.end()) {
        info.hasImports = true;
        info.importsValue = importsIt->second;
    }

    packageCache_[packageRoot] = info;
    out = info;
    return true;
}

bool ModuleResolver::findPackageRoot(const std::string& startDir,
                                     const std::string& packageName,
                                     std::string& outRoot) const {
    fs::path current = fs::path(startDir);
    if (!useBundle_) {
        current = fs::absolute(current).lexically_normal();
    }

    while (true) {
        fs::path candidate = current / "node_modules" / packageName;
        std::string pkgDir = candidate.lexically_normal().generic_string();

        if (dirExists(pkgDir)) {
            outRoot = pkgDir;
            return true;
        }

        if (current == current.root_path()) {
            break;
        }
        current = current.parent_path();
        if (current.empty()) {
            break;
        }
    }

    return false;
}

bool ModuleResolver::findNearestPackage(const std::string& startDir,
                                        std::string& outRoot) const {
    fs::path current = fs::path(startDir);
    if (!useBundle_) {
        current = fs::absolute(current).lexically_normal();
    }

    while (true) {
        fs::path pkgPath = current / "package.json";
        if (fileExists(pkgPath.generic_string())) {
            outRoot = current.generic_string();
            return true;
        }
        if (current == current.root_path()) {
            break;
        }
        current = current.parent_path();
        if (current.empty()) {
            break;
        }
    }

    return false;
}

ModuleFormat ModuleResolver::detectFormatForPath(const std::string& path) const {
    fs::path p(path);
    std::string ext = p.extension().string();
    if (ext == ".mjs" || ext == ".mts") {
        return ModuleFormat::ESM;
    }
    if (ext == ".cjs" || ext == ".cts") {
        return ModuleFormat::CJS;
    }
    if (ext == ".json") {
        return ModuleFormat::JSON;
    }
    if (ext == ".js" || ext == ".ts" || ext == ".tsx") {
        std::string type = detectPackageTypeForPath(path);
        if (type == "module") {
            return ModuleFormat::ESM;
        }
        return ModuleFormat::CJS;
    }
    return ModuleFormat::CJS;
}

std::string ModuleResolver::detectPackageTypeForPath(const std::string& path) const {
    std::string dir = dirname(path);
    std::string packageRoot;
    if (!findNearestPackage(dir, packageRoot)) {
        return "";
    }

    auto it = packageCache_.find(packageRoot);
    if (it != packageCache_.end()) {
        return it->second.type;
    }

    PackageInfo info;
    std::string error;
    if (loadPackageJson(packageRoot, info, error)) {
        return info.type;
    }
    return "";
}

bool ModuleResolver::readFile(const ResolvedPath& path, std::string& out, std::string& error) const {
    error.clear();
    out.clear();
    if (path.path == "mystral:shared") {
        out =
            "const api = globalThis.__mystralShared;\n"
            "if (!api) throw new Error('Mystral shared API is not initialized');\n"
            "export const SharedBuffer = api.SharedBuffer;\n"
            "export const SharedTable = api.SharedTable;\n"
            "export const SharedQueue = api.SharedQueue;\n"
            "export default api;\n";
        return true;
    }
    if (path.path == "mystral:worker-pool") {
        out =
            "const api = globalThis.__mystralWorkerPool;\n"
            "if (!api) throw new Error('Mystral WorkerPool API is not initialized');\n"
            "export const WorkerPool = api.WorkerPool;\n"
            "export const exposeWorkerTask = api.exposeWorkerTask;\n"
            "export const transferResult = api.transferResult;\n"
            "export const deterministicPartitions = api.deterministicPartitions;\n"
            "export default api;\n";
        return true;
    }
    if (path.isBundle) {
        std::vector<uint8_t> data;
        if (!vfs::readEmbeddedFile(path.path, data)) {
            error = "Bundle file not found: " + path.path;
            return false;
        }
        out.assign(data.begin(), data.end());
        return true;
    }

    std::ifstream file(path.path, std::ios::binary);
    if (!file.is_open()) {
        error = "Failed to open file: " + path.path;
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

bool ModuleResolver::fileExists(const std::string& path) const {
    if (useBundle_) {
        std::vector<uint8_t> data;
        return vfs::readEmbeddedFile(path, data);
    }
    std::error_code ec;
    return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}

bool ModuleResolver::dirExists(const std::string& path) const {
    if (useBundle_) {
        std::string pkgPath = (fs::path(path) / "package.json").string();
        if (fileExists(pkgPath)) {
            return true;
        }
        std::string indexJs = (fs::path(path) / "index.js").string();
        std::string indexMjs = (fs::path(path) / "index.mjs").string();
        std::string indexCjs = (fs::path(path) / "index.cjs").string();
        return fileExists(indexJs) || fileExists(indexMjs) || fileExists(indexCjs);
    }
    std::error_code ec;
    return fs::exists(path, ec) && fs::is_directory(path, ec);
}

bool ModuleResolver::resolveExportsTarget(const JsonValue& exportsValue,
                                          const std::string& subpath,
                                          const std::vector<std::string>& conditions,
                                          std::string& outTarget,
                                          std::string& error) const {
    if (exportsValue.type == JsonValue::Type::String) {
        if (subpath != "." && subpath != exportsValue.stringVal) {
            error = "No export defined for subpath: " + subpath;
            return false;
        }
        outTarget = exportsValue.stringVal;
        return true;
    }

    if (exportsValue.type == JsonValue::Type::Array) {
        for (const auto& entry : exportsValue.arrayVal) {
            std::string target;
            if (resolveExportsTarget(entry, subpath, conditions, target, error)) {
                outTarget = target;
                return true;
            }
        }
        return false;
    }

    if (exportsValue.type != JsonValue::Type::Object) {
        error = "Invalid exports definition";
        return false;
    }

    bool hasSubpathKeys = false;
    for (const auto& kv : exportsValue.objectVal) {
        if (isSubpathKey(kv.first)) {
            hasSubpathKeys = true;
            break;
        }
    }

    if (!hasSubpathKeys) {
        return resolveConditionalTarget(exportsValue, conditions, outTarget, error);
    }

    auto exact = exportsValue.objectVal.find(subpath);
    if (exact != exportsValue.objectVal.end()) {
        return resolveConditionalTarget(exact->second, conditions, outTarget, error);
    }

    for (const auto& kv : exportsValue.objectVal) {
        if (!isSubpathKey(kv.first)) {
            continue;
        }
        std::string mapped;
        if (applyExportsPattern(kv.first, subpath, mapped)) {
            std::string target;
            if (!resolveConditionalTarget(kv.second, conditions, target, error)) {
                return false;
            }
            if (mapped.find('*') != std::string::npos) {
                error = "Nested export pattern not supported";
                return false;
            }
            size_t star = target.find('*');
            if (star != std::string::npos) {
                target.replace(star, 1, mapped);
            }
            outTarget = target;
            return true;
        }
    }

    error = "No matching export for subpath: " + subpath;
    return false;
}

bool ModuleResolver::resolveConditionalTarget(const JsonValue& value,
                                              const std::vector<std::string>& conditions,
                                              std::string& outTarget,
                                              std::string& error) const {
    if (value.type == JsonValue::Type::String) {
        outTarget = value.stringVal;
        return true;
    }
    if (value.type == JsonValue::Type::Array) {
        for (const auto& entry : value.arrayVal) {
            if (resolveConditionalTarget(entry, conditions, outTarget, error)) {
                return true;
            }
        }
        return false;
    }
    if (value.type != JsonValue::Type::Object) {
        error = "Invalid conditional exports target";
        return false;
    }

    for (const auto& condition : conditions) {
        auto it = value.objectVal.find(condition);
        if (it != value.objectVal.end()) {
            return resolveConditionalTarget(it->second, conditions, outTarget, error);
        }
    }

    error = "No matching export condition";
    return false;
}

bool ModuleResolver::isSubpathKey(const std::string& key) const {
    return !key.empty() && (key[0] == '.' || key[0] == '/' || key[0] == '#');
}

bool ModuleResolver::applyExportsPattern(const std::string& pattern,
                                         const std::string& subpath,
                                         std::string& out) const {
    size_t star = pattern.find('*');
    if (star == std::string::npos) {
        return false;
    }
    std::string prefix = pattern.substr(0, star);
    std::string suffix = pattern.substr(star + 1);
    if (!startsWith(subpath, prefix)) {
        return false;
    }
    if (!suffix.empty() && subpath.size() >= suffix.size()) {
        if (subpath.compare(subpath.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return false;
        }
    }
    size_t middleStart = prefix.size();
    size_t middleEnd = subpath.size() - suffix.size();
    if (middleEnd < middleStart) {
        return false;
    }
    out = subpath.substr(middleStart, middleEnd - middleStart);
    return true;
}

bool ModuleResolver::parseJson(const std::string& input, JsonValue& out, std::string& error) const {
    size_t pos = 0;
    if (!parseJsonValue(input, pos, out, error)) {
        return false;
    }
    skipWhitespace(input, pos);
    if (pos != input.size()) {
        error = "Trailing characters in JSON";
        return false;
    }
    return true;
}

bool ModuleResolver::parseJsonValue(const std::string& input, size_t& pos, JsonValue& out, std::string& error) const {
    skipWhitespace(input, pos);
    if (pos >= input.size()) {
        error = "Unexpected end of JSON";
        return false;
    }

    char c = input[pos];
    if (c == '"') {
        out.type = JsonValue::Type::String;
        return parseJsonString(input, pos, out.stringVal, error);
    }
    if (c == '{') {
        return parseJsonObject(input, pos, out, error);
    }
    if (c == '[') {
        return parseJsonArray(input, pos, out, error);
    }
    if (startsWith(input.substr(pos), "true")) {
        out.type = JsonValue::Type::Bool;
        out.boolVal = true;
        pos += 4;
        return true;
    }
    if (startsWith(input.substr(pos), "false")) {
        out.type = JsonValue::Type::Bool;
        out.boolVal = false;
        pos += 5;
        return true;
    }
    if (startsWith(input.substr(pos), "null")) {
        out.type = JsonValue::Type::Null;
        pos += 4;
        return true;
    }

    if (c == '-' || isNumberChar(c)) {
        size_t start = pos;
        pos++;
        while (pos < input.size() && (isNumberChar(input[pos]) || input[pos] == '.' || input[pos] == 'e' || input[pos] == 'E' || input[pos] == '+' || input[pos] == '-')) {
            pos++;
        }
        std::string numStr = input.substr(start, pos - start);
        try {
            out.type = JsonValue::Type::Number;
            out.numberVal = std::stod(numStr);
            return true;
        } catch (...) {
            error = "Invalid number in JSON";
            return false;
        }
    }

    error = "Invalid JSON value";
    return false;
}

bool ModuleResolver::parseJsonObject(const std::string& input, size_t& pos, JsonValue& out, std::string& error) const {
    if (input[pos] != '{') {
        error = "Expected '{'";
        return false;
    }
    pos++;
    out.type = JsonValue::Type::Object;
    skipWhitespace(input, pos);
    if (pos < input.size() && input[pos] == '}') {
        pos++;
        return true;
    }
    while (pos < input.size()) {
        skipWhitespace(input, pos);
        std::string key;
        if (!parseJsonString(input, pos, key, error)) {
            return false;
        }
        skipWhitespace(input, pos);
        if (pos >= input.size() || input[pos] != ':') {
            error = "Expected ':' in object";
            return false;
        }
        pos++;
        JsonValue value;
        if (!parseJsonValue(input, pos, value, error)) {
            return false;
        }
        out.objectVal[key] = value;
        skipWhitespace(input, pos);
        if (pos >= input.size()) {
            error = "Unterminated object";
            return false;
        }
        if (input[pos] == '}') {
            pos++;
            return true;
        }
        if (input[pos] != ',') {
            error = "Expected ',' in object";
            return false;
        }
        pos++;
    }
    error = "Unterminated object";
    return false;
}

bool ModuleResolver::parseJsonArray(const std::string& input, size_t& pos, JsonValue& out, std::string& error) const {
    if (input[pos] != '[') {
        error = "Expected '['";
        return false;
    }
    pos++;
    out.type = JsonValue::Type::Array;
    skipWhitespace(input, pos);
    if (pos < input.size() && input[pos] == ']') {
        pos++;
        return true;
    }
    while (pos < input.size()) {
        JsonValue value;
        if (!parseJsonValue(input, pos, value, error)) {
            return false;
        }
        out.arrayVal.push_back(value);
        skipWhitespace(input, pos);
        if (pos >= input.size()) {
            error = "Unterminated array";
            return false;
        }
        if (input[pos] == ']') {
            pos++;
            return true;
        }
        if (input[pos] != ',') {
            error = "Expected ',' in array";
            return false;
        }
        pos++;
    }
    error = "Unterminated array";
    return false;
}

bool ModuleResolver::parseJsonString(const std::string& input, size_t& pos, std::string& out, std::string& error) const {
    if (input[pos] != '"') {
        error = "Expected string";
        return false;
    }
    pos++;
    std::ostringstream result;
    while (pos < input.size()) {
        char c = input[pos++];
        if (c == '"') {
            out = result.str();
            return true;
        }
        if (c == '\\') {
            if (pos >= input.size()) {
                error = "Invalid escape in string";
                return false;
            }
            char esc = input[pos++];
            switch (esc) {
                case '"': result << '"'; break;
                case '\\': result << '\\'; break;
                case '/': result << '/'; break;
                case 'b': result << '\b'; break;
                case 'f': result << '\f'; break;
                case 'n': result << '\n'; break;
                case 'r': result << '\r'; break;
                case 't': result << '\t'; break;
                case 'u': {
                    if (pos + 4 > input.size()) {
                        error = "Invalid unicode escape";
                        return false;
                    }
                    int codePoint = 0;
                    for (int i = 0; i < 4; i++) {
                        char hex = input[pos++];
                        codePoint <<= 4;
                        if (hex >= '0' && hex <= '9') {
                            codePoint += hex - '0';
                        } else if (hex >= 'a' && hex <= 'f') {
                            codePoint += hex - 'a' + 10;
                        } else if (hex >= 'A' && hex <= 'F') {
                            codePoint += hex - 'A' + 10;
                        } else {
                            error = "Invalid unicode escape";
                            return false;
                        }
                    }
                    if (codePoint <= 0x7F) {
                        result << static_cast<char>(codePoint);
                    }
                    break;
                }
                default:
                    error = "Invalid escape in string";
                    return false;
            }
        } else {
            result << c;
        }
    }
    error = "Unterminated string";
    return false;
}

void ModuleResolver::skipWhitespace(const std::string& input, size_t& pos) const {
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
        pos++;
    }
}

bool ModuleResolver::isNumberChar(char c) const {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

}  // namespace js
}  // namespace mystral
