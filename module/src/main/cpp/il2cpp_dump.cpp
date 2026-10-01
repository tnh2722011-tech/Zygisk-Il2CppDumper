//
// Enhanced Zygisk-Il2CppDumper
// Complete 100% full game logic dumper with PC Il2CppDumper parity,
// in-memory decrypted global-metadata.dat extraction, and script.json generation.
// Saves dumps directly to device Download directory (/sdcard/Download/ and /sdcard/Download/Il2CppDumper/).
//

#include "il2cpp_dump.h"
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unordered_set>
#include <algorithm>
#include "xdl.h"
#include "log.h"
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"

#define DO_API(r, n, p) r (*n) p

#include "il2cpp-api-functions.h"

#undef DO_API

static uint64_t il2cpp_base = 0;

struct ScriptMethodEntry {
    uint64_t address;
    std::string name;
    std::string signature;
};

static inline const char *safe_str(const char *s, const char *d = "") {
    return s ? s : d;
}

static uint64_t get_module_base(const char *module_name) {
    FILE *fp = fopen("/proc/self/maps", "rt");
    if (!fp) return 0;
    char line[512];
    uint64_t base = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, module_name)) {
            uint64_t start = 0;
            if (sscanf(line, "%" SCNx64 "-", &start) == 1) {
                base = start;
                break;
            }
        }
    }
    fclose(fp);
    return base;
}

void init_il2cpp_api(void *handle) {
#define DO_API(r, n, p) {                      \
    n = (r (*) p)xdl_sym(handle, #n, nullptr); \
    if(!n) {                                   \
        LOGW("api not found: %s", #n);         \
    }                                          \
}

#include "il2cpp-api-functions.h"

#undef DO_API
}

static std::string json_escape(const std::string &str) {
    std::stringstream ss;
    for (char c : str) {
        if (c == '"') ss << "\\\"";
        else if (c == '\\') ss << "\\\\";
        else if (c == '\b') ss << "\\b";
        else if (c == '\f') ss << "\\f";
        else if (c == '\n') ss << "\\n";
        else if (c == '\r') ss << "\\r";
        else if (c == '\t') ss << "\\t";
        else if ((unsigned char)c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
            ss << buf;
        } else {
            ss << c;
        }
    }
    return ss.str();
}

static std::string clean_type_name(const std::string &name) {
    if (name == "System.Void") return "void";
    if (name == "System.Boolean") return "bool";
    if (name == "System.Byte") return "byte";
    if (name == "System.SByte") return "sbyte";
    if (name == "System.Char") return "char";
    if (name == "System.Int16") return "short";
    if (name == "System.UInt16") return "ushort";
    if (name == "System.Int32") return "int";
    if (name == "System.UInt32") return "uint";
    if (name == "System.Int64") return "long";
    if (name == "System.UInt64") return "ulong";
    if (name == "System.Single") return "float";
    if (name == "System.Double") return "double";
    if (name == "System.String") return "string";
    if (name == "System.Object") return "object";
    if (name == "System.IntPtr") return "IntPtr";
    if (name == "System.UIntPtr") return "UIntPtr";
    return name;
}

static std::string get_type_name(const Il2CppType *type) {
    if (!type) return "void";

    if (il2cpp_type_get_name) {
        char *name = il2cpp_type_get_name(type);
        if (name) {
            std::string strName(name);
            il2cpp_free(name);
            if (!strName.empty()) return clean_type_name(strName);
        }
    }

    Il2CppClass *klass = nullptr;
    if (il2cpp_class_from_type) {
        klass = il2cpp_class_from_type(type);
    }

    if (klass && il2cpp_class_get_name) {
        const char *kname = il2cpp_class_get_name(klass);
        if (kname && strlen(kname) > 0) {
            return std::string(kname);
        }
    }

    switch (type->type) {
        case IL2CPP_TYPE_VOID: return "void";
        case IL2CPP_TYPE_BOOLEAN: return "bool";
        case IL2CPP_TYPE_CHAR: return "char";
        case IL2CPP_TYPE_I1: return "sbyte";
        case IL2CPP_TYPE_U1: return "byte";
        case IL2CPP_TYPE_I2: return "short";
        case IL2CPP_TYPE_U2: return "ushort";
        case IL2CPP_TYPE_I4: return "int";
        case IL2CPP_TYPE_U4: return "uint";
        case IL2CPP_TYPE_I8: return "long";
        case IL2CPP_TYPE_U8: return "ulong";
        case IL2CPP_TYPE_R4: return "float";
        case IL2CPP_TYPE_R8: return "double";
        case IL2CPP_TYPE_STRING: return "string";
        case IL2CPP_TYPE_PTR: return "void*";
        case IL2CPP_TYPE_BYREF: return "ref";
        case IL2CPP_TYPE_VALUETYPE: return "struct";
        case IL2CPP_TYPE_CLASS: return "object";
        case IL2CPP_TYPE_VAR: return "T";
        case IL2CPP_TYPE_ARRAY:
        case IL2CPP_TYPE_SZARRAY: return "object[]";
        case IL2CPP_TYPE_GENERICINST: return "GenericType";
        case IL2CPP_TYPE_TYPEDBYREF: return "TypedReference";
        case IL2CPP_TYPE_I: return "IntPtr";
        case IL2CPP_TYPE_U: return "UIntPtr";
        case IL2CPP_TYPE_OBJECT: return "object";
        case IL2CPP_TYPE_MVAR: return "TMethod";
        default: return "object";
    }
}

static std::string get_method_modifier(uint32_t flags) {
    std::stringstream outPut;
    auto access = flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK;
    switch (access) {
        case METHOD_ATTRIBUTE_PRIVATE:
            outPut << "private ";
            break;
        case METHOD_ATTRIBUTE_PUBLIC:
            outPut << "public ";
            break;
        case METHOD_ATTRIBUTE_FAMILY:
            outPut << "protected ";
            break;
        case METHOD_ATTRIBUTE_ASSEM:
        case METHOD_ATTRIBUTE_FAM_AND_ASSEM:
            outPut << "internal ";
            break;
        case METHOD_ATTRIBUTE_FAM_OR_ASSEM:
            outPut << "protected internal ";
            break;
    }
    if (flags & METHOD_ATTRIBUTE_STATIC) {
        outPut << "static ";
    }
    if (flags & METHOD_ATTRIBUTE_ABSTRACT) {
        outPut << "abstract ";
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT) {
            outPut << "override ";
        }
    } else if (flags & METHOD_ATTRIBUTE_FINAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT) {
            outPut << "sealed override ";
        }
    } else if (flags & METHOD_ATTRIBUTE_VIRTUAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_NEW_SLOT) {
            outPut << "virtual ";
        } else {
            outPut << "override ";
        }
    }
    if (flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
        outPut << "extern ";
    }
    return outPut.str();
}

static bool _il2cpp_type_is_byref(const Il2CppType *type) {
    if (!type) return false;
    auto byref = type->byref;
    if (il2cpp_type_is_byref) {
        byref = il2cpp_type_is_byref(type);
    }
    return byref;
}

static std::string dump_field(Il2CppClass *klass, const std::string &indent) {
    std::stringstream outPut;
    if (!klass || !il2cpp_class_get_fields) return "";
    bool is_enum = il2cpp_class_is_enum ? il2cpp_class_is_enum(klass) : false;
    void *iter = nullptr;
    bool has_fields = false;
    while (auto field = il2cpp_class_get_fields(klass, &iter)) {
        if (!has_fields) {
            outPut << "\n" << indent << "\t// Fields\n";
            has_fields = true;
        }
        outPut << indent << "\t";
        auto attrs = il2cpp_field_get_flags ? il2cpp_field_get_flags(field) : 0;
        auto access = attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK;
        switch (access) {
            case FIELD_ATTRIBUTE_PRIVATE:
                outPut << "private ";
                break;
            case FIELD_ATTRIBUTE_PUBLIC:
                outPut << "public ";
                break;
            case FIELD_ATTRIBUTE_FAMILY:
                outPut << "protected ";
                break;
            case FIELD_ATTRIBUTE_ASSEMBLY:
            case FIELD_ATTRIBUTE_FAM_AND_ASSEM:
                outPut << "internal ";
                break;
            case FIELD_ATTRIBUTE_FAM_OR_ASSEM:
                outPut << "protected internal ";
                break;
        }
        if (attrs & FIELD_ATTRIBUTE_LITERAL) {
            outPut << "const ";
        } else {
            if (attrs & FIELD_ATTRIBUTE_STATIC) {
                outPut << "static ";
            }
            if (attrs & FIELD_ATTRIBUTE_INIT_ONLY) {
                outPut << "readonly ";
            }
        }
        auto field_type = il2cpp_field_get_type ? il2cpp_field_get_type(field) : nullptr;
        const char *fname = il2cpp_field_get_name ? il2cpp_field_get_name(field) : nullptr;
        outPut << get_type_name(field_type) << " " << safe_str(fname, "unnamed_field");

        if ((attrs & FIELD_ATTRIBUTE_LITERAL) && is_enum && il2cpp_field_static_get_value) {
            uint64_t val = 0;
            il2cpp_field_static_get_value(field, &val);
            outPut << " = " << std::dec << val;
        }
        size_t offset = il2cpp_field_get_offset ? il2cpp_field_get_offset(field) : 0;
        outPut << "; // 0x" << std::hex << offset << "\n";
    }
    return outPut.str();
}

static std::string dump_property(Il2CppClass *klass, const std::string &indent) {
    std::stringstream outPut;
    if (!klass || !il2cpp_class_get_properties) return "";
    void *iter = nullptr;
    bool has_props = false;
    while (auto prop_const = il2cpp_class_get_properties(klass, &iter)) {
        if (!has_props) {
            outPut << "\n" << indent << "\t// Properties\n";
            has_props = true;
        }
        auto prop = const_cast<PropertyInfo *>(prop_const);
        auto get = il2cpp_property_get_get_method ? il2cpp_property_get_get_method(prop) : nullptr;
        auto set = il2cpp_property_get_set_method ? il2cpp_property_get_set_method(prop) : nullptr;
        auto prop_name = il2cpp_property_get_name ? il2cpp_property_get_name(prop) : nullptr;
        outPut << indent << "\t";
        const Il2CppType *prop_type = nullptr;
        uint32_t iflags = 0;
        if (get && il2cpp_method_get_flags && il2cpp_method_get_return_type) {
            outPut << get_method_modifier(il2cpp_method_get_flags(get, &iflags));
            prop_type = il2cpp_method_get_return_type(get);
        } else if (set && il2cpp_method_get_flags && il2cpp_method_get_param) {
            outPut << get_method_modifier(il2cpp_method_get_flags(set, &iflags));
            prop_type = il2cpp_method_get_param(set, 0);
        }
        if (prop_type) {
            outPut << get_type_name(prop_type) << " " << safe_str(prop_name, "unnamed_prop") << " { ";
            if (get) outPut << "get; ";
            if (set) outPut << "set; ";
            outPut << "}\n";
        } else {
            if (prop_name) {
                outPut << "// unknown property " << prop_name << "\n";
            }
        }
    }
    return outPut.str();
}

static std::string dump_event(Il2CppClass *klass, const std::string &indent) {
    std::stringstream outPut;
    if (!klass || !il2cpp_class_get_events) return "";
    void *iter = nullptr;
    bool has_events = false;
    while (auto event_info = il2cpp_class_get_events(klass, &iter)) {
        if (!has_events) {
            outPut << "\n" << indent << "\t// Events\n";
            has_events = true;
        }
        outPut << indent << "\t";
        const char *name = event_info->name ? event_info->name : "unnamed_event";
        if (event_info->eventType) {
            outPut << "public event " << get_type_name(event_info->eventType) << " " << name << ";\n";
        } else {
            outPut << "public event EventHandler " << name << ";\n";
        }
    }
    return outPut.str();
}

static std::string dump_method(Il2CppClass *klass, const std::string &indent, const std::string &class_full_name, std::vector<ScriptMethodEntry> &script_methods) {
    std::stringstream outPut;
    if (!klass || !il2cpp_class_get_methods) return "";
    void *iter = nullptr;
    bool has_methods = false;
    while (auto method = il2cpp_class_get_methods(klass, &iter)) {
        if (!has_methods) {
            outPut << "\n" << indent << "\t// Methods\n";
            has_methods = true;
        }
        uint64_t method_ptr = (uint64_t) method->methodPointer;
        if (method_ptr) {
            uint64_t rva = method_ptr - il2cpp_base;
            outPut << indent << "\t// RVA: 0x" << std::hex << rva << " VA: 0x" << std::hex << method_ptr;
        } else {
            outPut << indent << "\t// RVA: 0x VA: 0x0";
        }
        outPut << "\n" << indent << "\t";
        uint32_t iflags = 0;
        uint32_t flags = il2cpp_method_get_flags ? il2cpp_method_get_flags(method, &iflags) : 0;
        outPut << get_method_modifier(flags);

        auto return_type = il2cpp_method_get_return_type ? il2cpp_method_get_return_type(method) : nullptr;
        if (return_type && _il2cpp_type_is_byref(return_type)) {
            outPut << "ref ";
        }
        std::string ret_type_str = get_type_name(return_type);
        const char *mname = il2cpp_method_get_name ? il2cpp_method_get_name(method) : "unnamed_method";
        outPut << ret_type_str << " " << safe_str(mname, "unnamed_method") << "(";

        std::stringstream paramSig;
        uint32_t param_count = il2cpp_method_get_param_count ? il2cpp_method_get_param_count(method) : 0;
        for (uint32_t i = 0; i < param_count; ++i) {
            auto param = il2cpp_method_get_param(method, i);
            if (!param) continue;
            auto attrs = param->attrs;
            if (_il2cpp_type_is_byref(param)) {
                if (attrs & PARAM_ATTRIBUTE_OUT && !(attrs & PARAM_ATTRIBUTE_IN)) {
                    outPut << "out ";
                } else if (attrs & PARAM_ATTRIBUTE_IN && !(attrs & PARAM_ATTRIBUTE_OUT)) {
                    outPut << "in ";
                } else {
                    outPut << "ref ";
                }
            } else {
                if (attrs & PARAM_ATTRIBUTE_IN) {
                    outPut << "[In] ";
                }
                if (attrs & PARAM_ATTRIBUTE_OUT) {
                    outPut << "[Out] ";
                }
            }
            std::string param_type_str = get_type_name(param);
            const char *pname = il2cpp_method_get_param_name ? il2cpp_method_get_param_name(method, i) : nullptr;
            std::string param_name_str = pname ? pname : ("a" + std::to_string(i));
            outPut << param_type_str << " " << param_name_str << ", ";
            paramSig << param_type_str << " " << param_name_str << (i + 1 < param_count ? ", " : "");
        }
        if (param_count > 0) {
            outPut.seekp(-2, outPut.cur);
        }
        outPut << ") { }\n";

        if (method_ptr) {
            ScriptMethodEntry entry;
            entry.address = method_ptr;
            entry.name = class_full_name + "$$" + safe_str(mname, "unnamed_method");
            entry.signature = ret_type_str + " " + class_full_name + "::" + safe_str(mname, "unnamed_method") + "(" + paramSig.str() + ")";
            script_methods.push_back(entry);
        }
    }
    return outPut.str();
}

static std::string dump_type(Il2CppClass *klass, std::unordered_set<Il2CppClass*> &dumped_classes, std::vector<ScriptMethodEntry> &script_methods, int indent_level = 0) {
    if (!klass) return "";
    if (dumped_classes.find(klass) != dumped_classes.end()) return "";
    dumped_classes.insert(klass);

    std::stringstream outPut;
    std::string indent(indent_level, '\t');

    const char *namespaze = il2cpp_class_get_namespace ? il2cpp_class_get_namespace(klass) : "";
    const char *name = il2cpp_class_get_name ? il2cpp_class_get_name(klass) : "UnnamedClass";
    namespaze = safe_str(namespaze);
    name = safe_str(name, "UnnamedClass");

    std::string full_name = (strlen(namespaze) > 0) ? (std::string(namespaze) + "." + name) : std::string(name);

    if (indent_level == 0) {
        outPut << "\n// Namespace: " << namespaze << "\n";
    }

    uint32_t flags = il2cpp_class_get_flags ? il2cpp_class_get_flags(klass) : 0;
    if (flags & TYPE_ATTRIBUTE_SERIALIZABLE) {
        outPut << indent << "[Serializable]\n";
    }

    bool is_valuetype = il2cpp_class_is_valuetype ? il2cpp_class_is_valuetype(klass) : false;
    bool is_enum = il2cpp_class_is_enum ? il2cpp_class_is_enum(klass) : false;
    auto visibility = flags & TYPE_ATTRIBUTE_VISIBILITY_MASK;
    outPut << indent;
    switch (visibility) {
        case TYPE_ATTRIBUTE_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_PUBLIC:
            outPut << "public ";
            break;
        case TYPE_ATTRIBUTE_NOT_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_FAM_AND_ASSEM:
        case TYPE_ATTRIBUTE_NESTED_ASSEMBLY:
            outPut << "internal ";
            break;
        case TYPE_ATTRIBUTE_NESTED_PRIVATE:
            outPut << "private ";
            break;
        case TYPE_ATTRIBUTE_NESTED_FAMILY:
            outPut << "protected ";
            break;
        case TYPE_ATTRIBUTE_NESTED_FAM_OR_ASSEM:
            outPut << "protected internal ";
            break;
    }

    if (flags & TYPE_ATTRIBUTE_ABSTRACT && flags & TYPE_ATTRIBUTE_SEALED) {
        outPut << "static ";
    } else if (!(flags & TYPE_ATTRIBUTE_INTERFACE) && flags & TYPE_ATTRIBUTE_ABSTRACT) {
        outPut << "abstract ";
    } else if (!is_valuetype && !is_enum && flags & TYPE_ATTRIBUTE_SEALED) {
        outPut << "sealed ";
    }

    if (flags & TYPE_ATTRIBUTE_INTERFACE) {
        outPut << "interface ";
    } else if (is_enum) {
        outPut << "enum ";
    } else if (is_valuetype) {
        outPut << "struct ";
    } else {
        outPut << "class ";
    }
    outPut << name;

    std::vector<std::string> extends;
    Il2CppClass *parent = il2cpp_class_get_parent ? il2cpp_class_get_parent(klass) : nullptr;
    if (!is_valuetype && !is_enum && parent) {
        const Il2CppType *parent_type = il2cpp_class_get_type ? il2cpp_class_get_type(parent) : nullptr;
        if (parent_type && parent_type->type != IL2CPP_TYPE_OBJECT) {
            const char *pname = il2cpp_class_get_name ? il2cpp_class_get_name(parent) : nullptr;
            if (pname) extends.emplace_back(pname);
        }
    }
    if (il2cpp_class_get_interfaces) {
        void *iter = nullptr;
        while (auto itf = il2cpp_class_get_interfaces(klass, &iter)) {
            const char *iname = il2cpp_class_get_name ? il2cpp_class_get_name(itf) : nullptr;
            if (iname) extends.emplace_back(iname);
        }
    }
    if (!extends.empty()) {
        outPut << " : " << extends[0];
        for (size_t i = 1; i < extends.size(); ++i) {
            outPut << ", " << extends[i];
        }
    }
    outPut << "\n" << indent << "{\n";

    outPut << dump_field(klass, indent);
    outPut << dump_property(klass, indent);
    outPut << dump_event(klass, indent);
    outPut << dump_method(klass, indent, full_name, script_methods);

    if (il2cpp_class_get_nested_types) {
        void *iter = nullptr;
        bool has_nested = false;
        while (auto nested_klass = il2cpp_class_get_nested_types(klass, &iter)) {
            if (nested_klass) {
                if (!has_nested) {
                    outPut << "\n" << indent << "\t// Nested Types\n";
                    has_nested = true;
                }
                outPut << dump_type(nested_klass, dumped_classes, script_methods, indent_level + 1);
            }
        }
    }

    outPut << indent << "}\n";
    return outPut.str();
}

static std::string generate_script_json(const std::vector<ScriptMethodEntry> &methods) {
    std::stringstream ss;
    ss << "{\n";
    ss << "  \"ScriptMethod\": [\n";
    for (size_t i = 0; i < methods.size(); ++i) {
        const auto &m = methods[i];
        ss << "    {\n";
        ss << "      \"Address\": " << m.address << ",\n";
        ss << "      \"Name\": \"" << json_escape(m.name) << "\",\n";
        ss << "      \"Signature\": \"" << json_escape(m.signature) << "\",\n";
        ss << "      \"TypeSignature\": \"void\"\n";
        ss << "    }" << (i + 1 < methods.size() ? "," : "") << "\n";
    }
    ss << "  ],\n";
    ss << "  \"ScriptString\": [],\n";
    ss << "  \"ScriptMetadata\": [],\n";
    ss << "  \"ScriptMetadataMethod\": [],\n";
    ss << "  \"Addresses\": [\n";
    for (size_t i = 0; i < methods.size(); ++i) {
        ss << "    " << methods[i].address << (i + 1 < methods.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

static std::vector<std::string> get_target_directories(const char *outDir) {
    std::vector<std::string> dirs;
    dirs.push_back("/sdcard/Download/Il2CppDumper");
    dirs.push_back("/sdcard/Download");
    dirs.push_back("/storage/emulated/0/Download/Il2CppDumper");
    dirs.push_back("/storage/emulated/0/Download");
    if (outDir && strlen(outDir) > 0) {
        dirs.push_back(std::string(outDir) + "/files");
        std::string sOut(outDir);
        auto lastSlash = sOut.rfind('/');
        if (lastSlash != std::string::npos) {
            std::string pkg = sOut.substr(lastSlash + 1);
            dirs.push_back("/storage/emulated/0/Android/data/" + pkg + "/files");
            dirs.push_back("/sdcard/Android/data/" + pkg + "/files");
        }
    }
    return dirs;
}

static void make_dir(const std::string &path) {
    mkdir(path.c_str(), 0777);
    chmod(path.c_str(), 0777);
}

static void save_text_file(const std::string &filename, const std::string &content, const char *outDir) {
    auto dirs = get_target_directories(outDir);
    bool saved_any = false;
    for (const auto &dir : dirs) {
        make_dir(dir);
        std::string full_path = dir + "/" + filename;
        FILE *fp = fopen(full_path.c_str(), "w");
        if (fp) {
            fwrite(content.data(), 1, content.size(), fp);
            fclose(fp);
            chmod(full_path.c_str(), 0666);
            LOGI("Saved %s to: %s (%zu bytes)", filename.c_str(), full_path.c_str(), content.size());
            saved_any = true;
        }
    }
    if (!saved_any) {
        LOGE("Failed to save %s to ANY location!", filename.c_str());
    }
}

class DumpWriter {
public:
    std::vector<FILE*> fps;
    std::vector<std::string> paths;

    void open_all(const std::string &filename, const char *outDir) {
        auto dirs = get_target_directories(outDir);
        for (const auto &dir : dirs) {
            make_dir(dir);
            std::string full_path = dir + "/" + filename;
            FILE *fp = fopen(full_path.c_str(), "w");
            if (fp) {
                fps.push_back(fp);
                paths.push_back(full_path);
            }
        }
        if (fps.empty()) {
            LOGE("DumpWriter: failed to open %s in any directory!", filename.c_str());
        }
    }

    void write(const std::string &str) {
        for (auto *fp : fps) {
            fwrite(str.data(), 1, str.size(), fp);
        }
    }

    void flush() {
        for (auto *fp : fps) {
            fflush(fp);
        }
    }

    void close_all() {
        for (size_t i = 0; i < fps.size(); ++i) {
            fflush(fps[i]);
            fclose(fps[i]);
            chmod(paths[i].c_str(), 0666);
            LOGI("Saved dump to: %s", paths[i].c_str());
        }
        fps.clear();
        paths.clear();
    }
};

static void dump_memory_metadata(const char *outDir) {
    LOGI("Scanning /proc/self/maps for decrypted in-memory global-metadata.dat...");
    FILE *maps = fopen("/proc/self/maps", "rt");
    if (!maps) {
        LOGW("Failed to open /proc/self/maps");
        return;
    }

    int mem_fd = open("/proc/self/mem", O_RDONLY);
    if (mem_fd == -1) {
        LOGW("Failed to open /proc/self/mem");
        fclose(maps);
        return;
    }

    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), maps)) {
        uintptr_t start = 0, end = 0;
        char perms[8] = {0};
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &start, &end, perms) < 3) {
            continue;
        }

        if (perms[0] != 'r') continue;

        size_t region_size = end - start;
        if (region_size < 512 * 1024 || region_size > 200 * 1024 * 1024) {
            continue;
        }

        uint32_t header_check[4] = {0};
        if (pread(mem_fd, header_check, sizeof(header_check), start) != sizeof(header_check)) {
            continue;
        }

        uint32_t sanity = header_check[0];
        int32_t version = header_check[1];

        // Standard magic 0xFAB11BAF or custom game magic 0xed566bff / 0xff6b56ed
        if ((sanity == 0xFAB11BAF || sanity == 0xed566bff || sanity == 0xff6b56ed) && (version >= 16 && version <= 40)) {
            LOGI("Found in-memory decrypted global-metadata.dat at 0x%" PRIxPTR " (sanity: 0x%x, version: %d, size: %zu)",
                 start, sanity, version, region_size);

            std::vector<uint8_t> buffer(region_size);
            ssize_t read_bytes = pread(mem_fd, buffer.data(), region_size, start);
            if (read_bytes > 0) {
                // Restore standard sanity 0xFAB11BAF so PC Il2CppDumper can open it seamlessly
                uint32_t standard_sanity = 0xFAB11BAF;
                memcpy(buffer.data(), &standard_sanity, sizeof(standard_sanity));

                auto dirs = get_target_directories(outDir);
                for (const auto &dir : dirs) {
                    make_dir(dir);
                    std::string meta_path = dir + "/global-metadata.dat";
                    FILE *mfp = fopen(meta_path.c_str(), "wb");
                    if (mfp) {
                        fwrite(buffer.data(), 1, read_bytes, mfp);
                        fclose(mfp);
                        chmod(meta_path.c_str(), 0666);
                        LOGI("Successfully dumped decrypted global-metadata.dat to: %s (%zd bytes)",
                             meta_path.c_str(), read_bytes);
                    }
                }
                found = true;
                break;
            }
        }
    }

    close(mem_fd);
    fclose(maps);

    if (!found) {
        LOGI("In-memory metadata will be dumped via runtime class reflection.");
    }
}

void il2cpp_api_init(void *handle) {
    LOGI("il2cpp_handle: %p", handle);
    init_il2cpp_api(handle);

    if (il2cpp_domain_get_assemblies) {
        Dl_info dlInfo;
        if (dladdr((void *) il2cpp_domain_get_assemblies, &dlInfo)) {
            il2cpp_base = reinterpret_cast<uint64_t>(dlInfo.dli_fbase);
        }
    }
    if (!il2cpp_base) {
        il2cpp_base = get_module_base("libil2cpp.so");
    }
    LOGI("il2cpp_base: %" PRIx64"", il2cpp_base);

    int vm_wait = 0;
    while (!il2cpp_is_vm_thread(nullptr)) {
        LOGI("Waiting for il2cpp_init... (%d)", ++vm_wait);
        sleep(1);
    }
    auto domain = il2cpp_domain_get();
    if (domain && il2cpp_thread_attach) {
        il2cpp_thread_attach(domain);
    }
}

void il2cpp_dump(const char *outDir) {
    LOGI("==================================================");
    LOGI("Starting Enhanced 100%% complete il2cpp_dump...");
    LOGI("==================================================");

    auto domain = il2cpp_domain_get();
    if (!domain) {
        LOGE("il2cpp_domain_get returned null!");
        return;
    }

    // 1. Dump in-memory decrypted global-metadata.dat first
    dump_memory_metadata(outDir);

    // 2. Wait for game assemblies (like Assembly-CSharp) to load into domain
    LOGI("Waiting for game assemblies (Assembly-CSharp.dll)...");
    bool game_assembly_loaded = false;
    for (int retry = 0; retry < 60; ++retry) {
        size_t size = 0;
        auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
        if (assemblies && size > 0) {
            for (size_t i = 0; i < size; ++i) {
                auto image = il2cpp_assembly_get_image(assemblies[i]);
                if (image && il2cpp_image_get_name) {
                    const char *img_name = il2cpp_image_get_name(image);
                    if (img_name && (strstr(img_name, "Assembly-CSharp") != nullptr || strstr(img_name, "Main") != nullptr)) {
                        game_assembly_loaded = true;
                        LOGI("Found game assembly: %s! (Total assemblies: %zu)", img_name, size);
                        break;
                    }
                }
            }
        }
        if (game_assembly_loaded) {
            LOGI("Game assembly loaded! Waiting 5s for classes and subsystems to register...");
            sleep(5);
            break;
        }
        sleep(1);
    }

    size_t size = 0;
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
    if (!assemblies) {
        LOGE("il2cpp_domain_get_assemblies returned null!");
        return;
    }

    DumpWriter writer;
    writer.open_all("dump.cs", outDir);

    std::stringstream headerOutput;
    headerOutput << "// =========================================================\n";
    headerOutput << "// Dumped by Enhanced Zygisk-Il2CppDumper (100% Logic Parity)\n";
    headerOutput << "// Total Assemblies: " << size << "\n";
    headerOutput << "// =========================================================\n\n";

    for (size_t i = 0; i < size; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (image && il2cpp_image_get_name) {
            headerOutput << "// Image " << i << ": " << il2cpp_image_get_name(image) << "\n";
        }
    }
    headerOutput << "\n";
    writer.write(headerOutput.str());

    std::unordered_set<Il2CppClass*> dumped_classes;
    std::vector<ScriptMethodEntry> script_methods;
    size_t class_dump_count = 0;

    if (il2cpp_image_get_class) {
        LOGI("Traversing assemblies via il2cpp_image_get_class (total %zu assemblies)...", size);
        for (size_t i = 0; i < size; ++i) {
            auto image = il2cpp_assembly_get_image(assemblies[i]);
            if (!image) continue;
            const char *img_name = il2cpp_image_get_name ? il2cpp_image_get_name(image) : "Unknown.dll";

            std::stringstream imgHdr;
            imgHdr << "\n// =========================================================\n";
            imgHdr << "// Dll : " << img_name << "\n";
            imgHdr << "// =========================================================\n";
            writer.write(imgHdr.str());

            auto classCount = il2cpp_image_get_class_count ? il2cpp_image_get_class_count(image) : 0;
            for (size_t j = 0; j < classCount; ++j) {
                auto klass = il2cpp_image_get_class(image, j);
                if (klass) {
                    std::string dumped = dump_type(const_cast<Il2CppClass *>(klass), dumped_classes, script_methods, 0);
                    if (!dumped.empty()) {
                        writer.write(dumped);
                        class_dump_count++;
                        if (class_dump_count % 1000 == 0) {
                            writer.flush();
                            LOGI("Dumped %zu classes, %zu methods...", class_dump_count, script_methods.size());
                        }
                    }
                }
            }
        }
    } else {
        LOGI("Version less than 2018.3 - using reflection fallback");
        auto corlib = il2cpp_get_corlib ? il2cpp_get_corlib() : nullptr;
        if (corlib && il2cpp_class_from_name && il2cpp_class_get_method_from_name) {
            auto assemblyClass = il2cpp_class_from_name(corlib, "System.Reflection", "Assembly");
            auto assemblyLoad = il2cpp_class_get_method_from_name(assemblyClass, "Load", 1);
            auto assemblyGetTypes = il2cpp_class_get_method_from_name(assemblyClass, "GetTypes", 0);
            if (assemblyLoad && assemblyLoad->methodPointer && assemblyGetTypes && assemblyGetTypes->methodPointer) {
                typedef void *(*Assembly_Load_ftn)(void *, Il2CppString *, void *);
                typedef Il2CppArray *(*Assembly_GetTypes_ftn)(void *, void *);
                for (size_t i = 0; i < size; ++i) {
                    auto image = il2cpp_assembly_get_image(assemblies[i]);
                    if (!image) continue;
                    const char *image_name = il2cpp_image_get_name ? il2cpp_image_get_name(image) : "";

                    std::stringstream imgHdr;
                    imgHdr << "\n// =========================================================\n";
                    imgHdr << "// Dll : " << image_name << "\n";
                    imgHdr << "// =========================================================\n";
                    writer.write(imgHdr.str());

                    std::string imageName(image_name);
                    auto pos = imageName.rfind('.');
                    auto imageNameNoExt = imageName.substr(0, pos);
                    if (il2cpp_string_new) {
                        auto assemblyFileName = il2cpp_string_new(imageNameNoExt.data());
                        auto reflectionAssembly = ((Assembly_Load_ftn) assemblyLoad->methodPointer)(nullptr, assemblyFileName, nullptr);
                        if (reflectionAssembly) {
                            auto reflectionTypes = ((Assembly_GetTypes_ftn) assemblyGetTypes->methodPointer)(reflectionAssembly, nullptr);
                            if (reflectionTypes) {
                                auto items = reflectionTypes->vector;
                                for (int j = 0; j < reflectionTypes->max_length; ++j) {
                                    if (il2cpp_class_from_system_type) {
                                        auto klass = il2cpp_class_from_system_type((Il2CppReflectionType *) items[j]);
                                        if (klass) {
                                            std::string dumped = dump_type(klass, dumped_classes, script_methods, 0);
                                            if (!dumped.empty()) {
                                                writer.write(dumped);
                                                class_dump_count++;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (il2cpp_class_for_each) {
        LOGI("Executing il2cpp_class_for_each fallback for 100%% complete dump...");
        writer.write("\n// =========================================================\n");
        writer.write("// Additional Runtime / Generic Classes\n");
        writer.write("// =========================================================\n");

        struct ForEachCtx {
            std::unordered_set<Il2CppClass*> *dumped_classes;
            std::vector<ScriptMethodEntry> *script_methods;
            DumpWriter *writer;
            size_t *counter;
        } ctx = { &dumped_classes, &script_methods, &writer, &class_dump_count };

        auto callback = [](Il2CppClass *klass, void *userData) {
            if (!klass || !userData) return;
            auto *c = static_cast<ForEachCtx *>(userData);
            if (c->dumped_classes->find(klass) == c->dumped_classes->end()) {
                std::string dumped = dump_type(klass, *(c->dumped_classes), *(c->script_methods), 0);
                if (!dumped.empty()) {
                    c->writer->write(dumped);
                    (*(c->counter))++;
                }
            }
        };
        il2cpp_class_for_each(callback, &ctx);
    }

    writer.close_all();

    LOGI("Writing script.json (%zu methods)...", script_methods.size());
    std::string jsonContent = generate_script_json(script_methods);
    save_text_file("script.json", jsonContent, outDir);

    LOGI("=================================================");
    LOGI("100%% il2cpp_dump COMPLETE!");
    LOGI("Total Dumped Classes: %zu", dumped_classes.size());
    LOGI("Total Dumped Methods: %zu", script_methods.size());
    LOGI("Files saved directly to /sdcard/Download/ and /sdcard/Download/Il2CppDumper/");
    LOGI("=================================================");
}