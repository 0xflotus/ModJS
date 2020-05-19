#include "js.h"
#include <mutex>
#include <libplatform/libplatform.h>
#include <fstream>
#include <streambuf>
#include <stack>
#include <experimental/filesystem>
#include "sha256.h"

void KeyDBExecuteCallback(const v8::FunctionCallbackInfo<v8::Value>& args);

thread_local v8::Isolate *isolate = nullptr;
thread_local v8::Persistent<v8::ObjectTemplate, v8::CopyablePersistentTraits<v8::ObjectTemplate>> tls_global;
thread_local v8::Persistent<v8::Context, v8::CopyablePersistentTraits<v8::Context>> tls_context;

void javascript_initialize()
{
    v8::V8::InitializeICUDefaultLocation("keydb-server");
    v8::V8::InitializeExternalStartupData("keydb-server");
    std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(platform.get());
    v8::V8::Initialize();
    platform.release();
}

void javascript_shutdown()
{
    v8::V8::Dispose();
}

class HotScript
{
    v8::Persistent<v8::Script> m_script;
    BYTE m_hash[SHA256_BLOCK_SIZE];

public:
    HotScript(const char *rgch, size_t cch, v8::Local<v8::Script> script)
        : m_script(isolate, script)
    {
        SHA256_CTX ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, (BYTE*)rgch, cch);
        sha256_final(&ctx, m_hash);
    }

    bool FGetScript(const char *rgch, size_t cch, v8::Local<v8::Script> *pscriptOut)
    {
        BYTE hashT[SHA256_BLOCK_SIZE];
        SHA256_CTX ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, (BYTE*)rgch, cch);
        sha256_final(&ctx, hashT);

        return FGetScript(hashT, pscriptOut);
    }

    bool FGetScript(const BYTE *hash, v8::Local<v8::Script> *pscriptOut)
    {
        if (memcmp(m_hash, hash, SHA256_BLOCK_SIZE))
            return false;

        *pscriptOut = v8::Local<v8::Script>::New(isolate, m_script);
        if (pscriptOut->IsEmpty())
            return false;
        return true;
    }
};

static void LogCallback(const v8::FunctionCallbackInfo<v8::Value>& args) 
{
    if (args.Length() < 1) return;
    v8::Isolate* isolate = args.GetIsolate();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Value> arg = args[0];
    v8::String::Utf8Value value(isolate, arg);
    printf("%s\n", *value);
}

template<typename T>
class StackPopper
{
    std::stack<T> &m_stack;
public:
    StackPopper(std::stack<T> &stack, T &&val)
        : m_stack(stack)
    {
        stack.push(val);
    }

    ~StackPopper()
    {
        m_stack.pop();
    }
};

std::experimental::filesystem::path find_module(std::experimental::filesystem::path name)
{
    std::experimental::filesystem::path path;

    auto trypath = name;
    trypath.append(".js");
    if (std::experimental::filesystem::exists(trypath))
        return trypath;

    std::experimental::filesystem::path wdir = std::experimental::filesystem::current_path();
    do
    {
        auto node_dir = wdir / "node_modules";
        if (std::experimental::filesystem::is_directory(node_dir))
        {
            trypath = (node_dir / name).concat(".js");
            if (std::experimental::filesystem::exists(trypath))
                return trypath;
            trypath = node_dir / name / "index.js";
            if (std::experimental::filesystem::exists(trypath))
                return trypath;
        }
        wdir = wdir.parent_path();
    } while (!wdir.empty());
}

static void RequireCallback(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    static thread_local std::stack<std::experimental::filesystem::path> stackpath;

    if (args.Length() != 1) return;
    v8::HandleScope scope(isolate);
    v8::Local<v8::Value> arg = args[0];
    v8::String::Utf8Value utf8Path(isolate, arg);
    
    std::experimental::filesystem::path path(*utf8Path);
    if (path.is_relative() && !stackpath.empty())
    {
        // We have to make this relative to the previous file
        auto trypath = stackpath.top().remove_filename() / path;
        if (std::experimental::filesystem::exists(trypath))
        {
            path = trypath;
        }
        else if (std::experimental::filesystem::exists(trypath.concat(".js")))
        {
            path = trypath;
        }
        else
        {
            trypath = stackpath.top().remove_filename() / path / "index.js";
            if (std::experimental::filesystem::exists(trypath))
                path = trypath;
        }
    }
    if (!std::experimental::filesystem::exists(path) && path.extension().empty() && path.filename() == path)
    {
        path = find_module(path);
    }

    std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    if (size == -1)
    {
        isolate->ThrowException(v8::String::NewFromUtf8(isolate, "File not found").ToLocalChecked());
        return; // Failed to read file
    }

    file.seekg(0, std::ios::beg);
    StackPopper popper(stackpath, std::move(path));

    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size))
    {
        isolate->ThrowException(v8::String::NewFromUtf8(isolate, "File not found").ToLocalChecked());
        return; // Failed to read file
    }
    
    v8::Local<v8::ObjectTemplate> global = v8::Local<v8::ObjectTemplate>::New(isolate, tls_global);
    v8::Local<v8::Context> context = v8::Context::New(isolate, nullptr, global);

    v8::ScriptOrigin origin(arg,      // specifier
        v8::Integer::New(isolate, 0),             // line offset
        v8::Integer::New(isolate, 0),             // column offset
        v8::False(isolate),                       // is cross origin
        v8::Local<v8::Integer>(),                 // script id
        v8::Local<v8::Value>(),                   // source map URL
        v8::False(isolate),                       // is opaque
        v8::False(isolate),                       // is WASM
        v8::True(isolate));                       // is ES6 module
    
    v8::Context::Scope context_scope(context);

    v8::Local<v8::String> source_text =
        v8::String::NewFromUtf8(isolate, buffer.data(),
                                v8::NewStringType::kNormal, buffer.size())
            .ToLocalChecked();

    v8::ScriptCompiler::Source source(source_text, origin);

    v8::Local<v8::Module> module;
    if (!v8::ScriptCompiler::CompileModule(isolate, &source).ToLocal(&module)) {
        return;
    }

    bool flagT;
    auto maybeInstantiated = module->InstantiateModule(context, [](v8::Local<v8::Context> context, // "main.mjs"
                                      v8::Local<v8::String> specifier, // "some thing"
                                      v8::Local<v8::Module> referrer) -> v8::MaybeLocal<v8::Module> {
        return v8::Local<v8::Module>();
    });
    if (!maybeInstantiated.To(&flagT) || !flagT)
        return;

    v8::Local<v8::Object> valglob = context->Global();
    v8::Local<v8::Object> valModule = v8::Local<v8::Object>::Cast(valglob->Get(context, v8::String::NewFromUtf8(isolate, "module").ToLocalChecked()).ToLocalChecked());
    auto strExport = v8::String::NewFromUtf8(isolate, "exports").ToLocalChecked();
    v8::Local<v8::Value> exports = valModule->Get(context, strExport).ToLocalChecked();
    auto maybeSet = valglob->Set(context, strExport, exports);
    if (!maybeSet.To(&flagT) || !flagT)
        return;

    v8::Local<v8::Value> result;
    if (module->Evaluate(context).ToLocal(&result)) {
        // Even though we got exports above, we have to get it again now that the module was evaluated,
        //  this is because the module could have changed the object
        auto maybeExports = valModule->Get(context, strExport);
        if (maybeExports.ToLocal(&exports))
            args.GetReturnValue().Set(exports);
    }
}


void javascript_hooks_initialize(v8::Local<v8::ObjectTemplate> &keydb_obj)
{
    keydb_obj->Set(v8::String::NewFromUtf8(isolate, "log", v8::NewStringType::kNormal)
        .ToLocalChecked(),
        v8::FunctionTemplate::New(isolate, LogCallback));

    keydb_obj->Set(v8::String::NewFromUtf8(isolate, "call", v8::NewStringType::kNormal)
        .ToLocalChecked(),
        v8::FunctionTemplate::New(isolate, KeyDBExecuteCallback));
}

void javascript_thread_initialize()
{
    v8::Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    isolate = v8::Isolate::New(create_params);

    v8::HandleScope handle_scope(isolate);

    // Create a template for the global object where we set the
    // built-in global functions.
    v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
    v8::Local<v8::ObjectTemplate> keydb_obj = v8::ObjectTemplate::New(isolate);

    javascript_hooks_initialize(keydb_obj);

    global->Set(v8::String::NewFromUtf8(isolate, "keydb", v8::NewStringType::kNormal)
                    .ToLocalChecked(),
                keydb_obj);
    global->Set(v8::String::NewFromUtf8(isolate, "redis", v8::NewStringType::kNormal)
                    .ToLocalChecked(),
                keydb_obj);
    global->Set(v8::String::NewFromUtf8(isolate, "require", v8::NewStringType::kNormal)
                    .ToLocalChecked(),
                v8::FunctionTemplate::New(isolate, RequireCallback));

    v8::Local<v8::ObjectTemplate> module = v8::ObjectTemplate::New(isolate);
    module->Set(v8::String::NewFromUtf8(isolate, "exports", v8::NewStringType::kNormal)
                    .ToLocalChecked(),
                v8::ObjectTemplate::New(isolate));
    global->Set(v8::String::NewFromUtf8(isolate, "module", v8::NewStringType::kNormal)
                    .ToLocalChecked(),
                module);

    tls_global = v8::Persistent<v8::ObjectTemplate, v8::CopyablePersistentTraits<v8::ObjectTemplate>>(isolate, global);
    tls_context = v8::Persistent<v8::Context, v8::CopyablePersistentTraits<v8::Context>>(isolate, v8::Context::New(isolate, nullptr, global));
}

std::string prettyPrintException(v8::TryCatch &trycatch)
{
    auto e = trycatch.Exception();
    v8::Local<v8::Context> context = v8::Local<v8::Context>::New(isolate, isolate->GetCurrentContext());
    v8::String::Utf8Value estr(isolate, e);
    std::string str(*estr);

    auto maybeTrace = trycatch.StackTrace(context);
    v8::Local<v8::Value> traceV;

    if (maybeTrace.ToLocal(&traceV))
    {
        str += "\n";
        str += *v8::String::Utf8Value(isolate, traceV);
    }
    return str;
}

v8::Local<v8::Value> javascript_run(v8::Local<v8::Context> &context, v8::Local<v8::Script> &script)
{
    // Run the script to get the result.
    v8::Context::Scope context_scope(context);
    v8::TryCatch trycatch(isolate);

    v8::Local<v8::Value> result;
    auto resultMaybe = script->Run(context);

    if (!resultMaybe.ToLocal(&result))
    {
        if (trycatch.HasCaught())
        {
            throw prettyPrintException(trycatch);
        }
        throw std::nullptr_t();
    }
    
    // Convert the result to a KeyDB type and return it
    return result;
}

v8::Local<v8::Value> javascript_run(v8::Local<v8::Context> &context, const char *rgch, size_t cch)
{
    static thread_local HotScript *hotscript = nullptr;
    v8::TryCatch trycatch(isolate);
    
    // Enter the context for compiling and running the hello world script.
    v8::Context::Scope context_scope(context);

    v8::Local<v8::Script> script;
    if (hotscript == nullptr || !hotscript->FGetScript(rgch, cch, &script))
    {
        // Create a string containing the JavaScript source code.
        v8::Local<v8::String> sourceText =
            v8::String::NewFromUtf8(isolate, rgch,
                                    v8::NewStringType::kInternalized, cch)
                .ToLocalChecked();

        v8::MaybeLocal<v8::Script> scriptMaybe;
        v8::ScriptCompiler::Source source(sourceText);

        // Compile the source code.
        scriptMaybe = v8::ScriptCompiler::Compile(context, &source);

        if (!scriptMaybe.ToLocal(&script))
        {
            if (trycatch.HasCaught())
            {
                throw prettyPrintException(trycatch);
            }
            throw std::nullptr_t();
        }
        delete hotscript;
        hotscript = new HotScript(rgch, cch, script);        
    }
    return javascript_run(context, script);
}

void javascript_thread_shutdown()
{
    isolate->Dispose();
}
