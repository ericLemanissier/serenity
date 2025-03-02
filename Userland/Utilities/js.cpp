/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2020-2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Stream.h>
#include <LibCore/System.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Console.h>
#include <LibJS/Interpreter.h>
#include <LibJS/Parser.h>
#include <LibJS/Print.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/StringPrototype.h>
#include <LibJS/SourceTextModule.h>
#include <LibLine/Editor.h>
#include <LibMain/Main.h>
#include <LibTextCodec/Decoder.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

RefPtr<JS::VM> g_vm;
Vector<DeprecatedString> g_repl_statements;
JS::Handle<JS::Value> g_last_value = JS::make_handle(JS::js_undefined());

class ReplObject final : public JS::GlobalObject {
    JS_OBJECT(ReplObject, JS::GlobalObject);

public:
    ReplObject(JS::Realm& realm)
        : GlobalObject(realm)
    {
    }
    virtual void initialize(JS::Realm&) override;
    virtual ~ReplObject() override = default;

private:
    JS_DECLARE_NATIVE_FUNCTION(exit_interpreter);
    JS_DECLARE_NATIVE_FUNCTION(repl_help);
    JS_DECLARE_NATIVE_FUNCTION(save_to_file);
    JS_DECLARE_NATIVE_FUNCTION(load_ini);
    JS_DECLARE_NATIVE_FUNCTION(load_json);
    JS_DECLARE_NATIVE_FUNCTION(last_value_getter);
    JS_DECLARE_NATIVE_FUNCTION(print);
};

class ScriptObject final : public JS::GlobalObject {
    JS_OBJECT(ScriptObject, JS::GlobalObject);

public:
    ScriptObject(JS::Realm& realm)
        : JS::GlobalObject(realm)
    {
    }
    virtual void initialize(JS::Realm&) override;
    virtual ~ScriptObject() override = default;

private:
    JS_DECLARE_NATIVE_FUNCTION(load_ini);
    JS_DECLARE_NATIVE_FUNCTION(load_json);
    JS_DECLARE_NATIVE_FUNCTION(print);
};

static bool s_dump_ast = false;
static bool s_run_bytecode = false;
static bool s_opt_bytecode = false;
static bool s_as_module = false;
static bool s_print_last_result = false;
static bool s_strip_ansi = false;
static bool s_disable_source_location_hints = false;
static RefPtr<Line::Editor> s_editor;
static DeprecatedString s_history_path = DeprecatedString::formatted("{}/.js-history", Core::StandardPaths::home_directory());
static int s_repl_line_level = 0;
static bool s_fail_repl = false;

static ErrorOr<void> print(JS::Value value, Core::Stream::Stream& stream)
{
    JS::PrintContext print_context { .vm = *g_vm, .stream = stream, .strip_ansi = s_strip_ansi };
    return JS::print(value, print_context);
}

enum class PrintTarget {
    StandardError,
    StandardOutput,
};

static ErrorOr<void> print(JS::Value value, PrintTarget target = PrintTarget::StandardOutput)
{
    auto stream = TRY(target == PrintTarget::StandardError ? Core::Stream::File::standard_error() : Core::Stream::File::standard_output());
    return print(value, *stream);
}

static DeprecatedString prompt_for_level(int level)
{
    static StringBuilder prompt_builder;
    prompt_builder.clear();
    prompt_builder.append("> "sv);

    for (auto i = 0; i < level; ++i)
        prompt_builder.append("    "sv);

    return prompt_builder.build();
}

static DeprecatedString read_next_piece()
{
    StringBuilder piece;

    auto line_level_delta_for_next_line { 0 };

    do {
        auto line_result = s_editor->get_line(prompt_for_level(s_repl_line_level));

        line_level_delta_for_next_line = 0;

        if (line_result.is_error()) {
            s_fail_repl = true;
            return "";
        }

        auto& line = line_result.value();
        s_editor->add_to_history(line);

        piece.append(line);
        piece.append('\n');
        auto lexer = JS::Lexer(line);

        enum {
            NotInLabelOrObjectKey,
            InLabelOrObjectKeyIdentifier,
            InLabelOrObjectKey
        } label_state { NotInLabelOrObjectKey };

        for (JS::Token token = lexer.next(); token.type() != JS::TokenType::Eof; token = lexer.next()) {
            switch (token.type()) {
            case JS::TokenType::BracketOpen:
            case JS::TokenType::CurlyOpen:
            case JS::TokenType::ParenOpen:
                label_state = NotInLabelOrObjectKey;
                s_repl_line_level++;
                break;
            case JS::TokenType::BracketClose:
            case JS::TokenType::CurlyClose:
            case JS::TokenType::ParenClose:
                label_state = NotInLabelOrObjectKey;
                s_repl_line_level--;
                break;

            case JS::TokenType::Identifier:
            case JS::TokenType::StringLiteral:
                if (label_state == NotInLabelOrObjectKey)
                    label_state = InLabelOrObjectKeyIdentifier;
                else
                    label_state = NotInLabelOrObjectKey;
                break;
            case JS::TokenType::Colon:
                if (label_state == InLabelOrObjectKeyIdentifier)
                    label_state = InLabelOrObjectKey;
                else
                    label_state = NotInLabelOrObjectKey;
                break;
            default:
                break;
            }
        }

        if (label_state == InLabelOrObjectKey) {
            // If there's a label or object literal key at the end of this line,
            // prompt for more lines but do not change the line level.
            line_level_delta_for_next_line += 1;
        }
    } while (s_repl_line_level + line_level_delta_for_next_line > 0);

    return piece.to_deprecated_string();
}

static bool write_to_file(DeprecatedString const& path)
{
    int fd = open(path.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    for (size_t i = 0; i < g_repl_statements.size(); i++) {
        auto line = g_repl_statements[i];
        if (line.length() && i != g_repl_statements.size() - 1) {
            ssize_t nwritten = write(fd, line.characters(), line.length());
            if (nwritten < 0) {
                close(fd);
                return false;
            }
        }
        if (i != g_repl_statements.size() - 1) {
            char ch = '\n';
            ssize_t nwritten = write(fd, &ch, 1);
            if (nwritten != 1) {
                perror("write");
                close(fd);
                return false;
            }
        }
    }
    close(fd);
    return true;
}

static ErrorOr<bool> parse_and_run(JS::Interpreter& interpreter, StringView source, StringView source_name)
{
    enum class ReturnEarly {
        No,
        Yes,
    };

    JS::ThrowCompletionOr<JS::Value> result { JS::js_undefined() };

    auto run_script_or_module = [&](auto& script_or_module) {
        if (s_dump_ast)
            script_or_module->parse_node().dump(0);

        if (JS::Bytecode::g_dump_bytecode || s_run_bytecode) {
            auto executable_result = JS::Bytecode::Generator::generate(script_or_module->parse_node());
            if (executable_result.is_error()) {
                result = g_vm->throw_completion<JS::InternalError>(executable_result.error().to_deprecated_string());
                return ReturnEarly::No;
            }

            auto executable = executable_result.release_value();
            executable->name = source_name;
            if (s_opt_bytecode) {
                auto& passes = JS::Bytecode::Interpreter::optimization_pipeline(JS::Bytecode::Interpreter::OptimizationLevel::Optimize);
                passes.perform(*executable);
                dbgln("Optimisation passes took {}us", passes.elapsed());
            }

            if (JS::Bytecode::g_dump_bytecode)
                executable->dump();

            if (s_run_bytecode) {
                JS::Bytecode::Interpreter bytecode_interpreter(interpreter.realm());
                auto result_or_error = bytecode_interpreter.run_and_return_frame(*executable, nullptr);
                if (result_or_error.value.is_error())
                    result = result_or_error.value.release_error();
                else
                    result = result_or_error.frame->registers[0];
            } else {
                return ReturnEarly::Yes;
            }
        } else {
            result = interpreter.run(*script_or_module);
        }

        return ReturnEarly::No;
    };

    if (!s_as_module) {
        auto script_or_error = JS::Script::parse(source, interpreter.realm(), source_name);
        if (script_or_error.is_error()) {
            auto error = script_or_error.error()[0];
            auto hint = error.source_location_hint(source);
            if (!hint.is_empty())
                outln("{}", hint);
            outln("{}", error.to_deprecated_string());
            result = interpreter.vm().throw_completion<JS::SyntaxError>(error.to_deprecated_string());
        } else {
            auto return_early = run_script_or_module(script_or_error.value());
            if (return_early == ReturnEarly::Yes)
                return true;
        }
    } else {
        auto module_or_error = JS::SourceTextModule::parse(source, interpreter.realm(), source_name);
        if (module_or_error.is_error()) {
            auto error = module_or_error.error()[0];
            auto hint = error.source_location_hint(source);
            if (!hint.is_empty())
                outln("{}", hint);
            outln(error.to_deprecated_string());
            result = interpreter.vm().throw_completion<JS::SyntaxError>(error.to_deprecated_string());
        } else {
            auto return_early = run_script_or_module(module_or_error.value());
            if (return_early == ReturnEarly::Yes)
                return true;
        }
    }

    auto handle_exception = [&](JS::Value thrown_value) -> ErrorOr<void> {
        warnln("Uncaught exception: ");
        TRY(print(thrown_value, PrintTarget::StandardError));
        warnln();

        if (!thrown_value.is_object() || !is<JS::Error>(thrown_value.as_object()))
            return {};
        auto& traceback = static_cast<JS::Error const&>(thrown_value.as_object()).traceback();
        if (traceback.size() > 1) {
            unsigned repetitions = 0;
            for (size_t i = 0; i < traceback.size(); ++i) {
                auto& traceback_frame = traceback[i];
                if (i + 1 < traceback.size()) {
                    auto& next_traceback_frame = traceback[i + 1];
                    if (next_traceback_frame.function_name == traceback_frame.function_name) {
                        repetitions++;
                        continue;
                    }
                }
                if (repetitions > 4) {
                    // If more than 5 (1 + >4) consecutive function calls with the same name, print
                    // the name only once and show the number of repetitions instead. This prevents
                    // printing ridiculously large call stacks of recursive functions.
                    warnln(" -> {}", traceback_frame.function_name);
                    warnln(" {} more calls", repetitions);
                } else {
                    for (size_t j = 0; j < repetitions + 1; ++j)
                        warnln(" -> {}", traceback_frame.function_name);
                }
                repetitions = 0;
            }
        }
        return {};
    };

    if (!result.is_error())
        g_last_value = JS::make_handle(result.value());

    if (result.is_error()) {
        VERIFY(result.throw_completion().value().has_value());
        TRY(handle_exception(*result.release_error().value()));
        return false;
    }

    if (s_print_last_result) {
        TRY(print(result.value()));
        warnln();
    }

    return true;
}

static JS::ThrowCompletionOr<JS::Value> load_ini_impl(JS::VM& vm)
{
    auto& realm = *vm.current_realm();

    auto filename = TRY(vm.argument(0).to_string(vm));
    auto file_or_error = Core::Stream::File::open(filename, Core::Stream::OpenMode::Read);
    if (file_or_error.is_error())
        return vm.throw_completion<JS::Error>(DeprecatedString::formatted("Failed to open '{}': {}", filename, file_or_error.error()));

    auto config_file = MUST(Core::ConfigFile::open(filename, file_or_error.release_value()));
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    for (auto const& group : config_file->groups()) {
        auto group_object = JS::Object::create(realm, realm.intrinsics().object_prototype());
        for (auto const& key : config_file->keys(group)) {
            auto entry = config_file->read_entry(group, key);
            group_object->define_direct_property(key, JS::PrimitiveString::create(vm, move(entry)), JS::Attribute::Enumerable | JS::Attribute::Configurable | JS::Attribute::Writable);
        }
        object->define_direct_property(group, group_object, JS::Attribute::Enumerable | JS::Attribute::Configurable | JS::Attribute::Writable);
    }
    return object;
}

static JS::ThrowCompletionOr<JS::Value> load_json_impl(JS::VM& vm)
{
    auto filename = TRY(vm.argument(0).to_string(vm));
    auto file_or_error = Core::Stream::File::open(filename, Core::Stream::OpenMode::Read);
    if (file_or_error.is_error())
        return vm.throw_completion<JS::Error>(DeprecatedString::formatted("Failed to open '{}': {}", filename, file_or_error.error()));

    auto file_contents_or_error = file_or_error.value()->read_until_eof();
    if (file_contents_or_error.is_error())
        return vm.throw_completion<JS::Error>(DeprecatedString::formatted("Failed to read '{}': {}", filename, file_contents_or_error.error()));

    auto json = JsonValue::from_string(file_contents_or_error.value());
    if (json.is_error())
        return vm.throw_completion<JS::SyntaxError>(JS::ErrorType::JsonMalformed);

    return JS::JSONObject::parse_json_value(vm, json.value());
}

void ReplObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    define_direct_property("global", this, JS::Attribute::Enumerable);
    u8 attr = JS::Attribute::Configurable | JS::Attribute::Writable | JS::Attribute::Enumerable;
    define_native_function(realm, "exit", exit_interpreter, 0, attr);
    define_native_function(realm, "help", repl_help, 0, attr);
    define_native_function(realm, "save", save_to_file, 1, attr);
    define_native_function(realm, "loadINI", load_ini, 1, attr);
    define_native_function(realm, "loadJSON", load_json, 1, attr);
    define_native_function(realm, "print", print, 1, attr);

    define_native_accessor(
        realm,
        "_",
        [](JS::VM&) {
            return g_last_value.value();
        },
        [](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto& global_object = vm.get_global_object();
            VERIFY(is<ReplObject>(global_object));
            outln("Disable writing last value to '_'");

            // We must delete first otherwise this setter gets called recursively.
            TRY(global_object.internal_delete(JS::PropertyKey { "_" }));

            auto value = vm.argument(0);
            TRY(global_object.internal_set(JS::PropertyKey { "_" }, value, &global_object));
            return value;
        },
        attr);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::save_to_file)
{
    if (!vm.argument_count())
        return JS::Value(false);
    DeprecatedString save_path = vm.argument(0).to_string_without_side_effects();
    if (write_to_file(save_path)) {
        return JS::Value(true);
    }
    return JS::Value(false);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::exit_interpreter)
{
    if (!vm.argument_count())
        exit(0);
    exit(TRY(vm.argument(0).to_number(vm)).as_double());
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::repl_help)
{
    warnln("REPL commands:");
    warnln("    exit(code): exit the REPL with specified code. Defaults to 0.");
    warnln("    help(): display this menu");
    warnln("    loadINI(file): load the given file as INI.");
    warnln("    loadJSON(file): load the given file as JSON.");
    warnln("    print(value): pretty-print the given JS value.");
    warnln("    save(file): write REPL input history to the given file. For example: save(\"foo.txt\")");
    return JS::js_undefined();
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::load_ini)
{
    return load_ini_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::load_json)
{
    return load_json_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::print)
{
    auto result = ::print(vm.argument(0));
    if (result.is_error())
        return g_vm->throw_completion<JS::InternalError>(DeprecatedString::formatted("Failed to print value: {}", result.error()));

    outln();

    return JS::js_undefined();
}

void ScriptObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    define_direct_property("global", this, JS::Attribute::Enumerable);
    u8 attr = JS::Attribute::Configurable | JS::Attribute::Writable | JS::Attribute::Enumerable;
    define_native_function(realm, "loadINI", load_ini, 1, attr);
    define_native_function(realm, "loadJSON", load_json, 1, attr);
    define_native_function(realm, "print", print, 1, attr);
}

JS_DEFINE_NATIVE_FUNCTION(ScriptObject::load_ini)
{
    return load_ini_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ScriptObject::load_json)
{
    return load_json_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ScriptObject::print)
{
    auto result = ::print(vm.argument(0));
    if (result.is_error())
        return g_vm->throw_completion<JS::InternalError>(DeprecatedString::formatted("Failed to print value: {}", result.error()));

    outln();

    return JS::js_undefined();
}

static ErrorOr<void> repl(JS::Interpreter& interpreter)
{
    while (!s_fail_repl) {
        DeprecatedString piece = read_next_piece();
        if (Utf8View { piece }.trim(JS::whitespace_characters).is_empty())
            continue;

        g_repl_statements.append(piece);
        TRY(parse_and_run(interpreter, piece, "REPL"sv));
    }
    return {};
}

static Function<void()> interrupt_interpreter;
static void sigint_handler()
{
    interrupt_interpreter();
}

class ReplConsoleClient final : public JS::ConsoleClient {
public:
    ReplConsoleClient(JS::Console& console)
        : ConsoleClient(console)
    {
    }

    virtual void clear() override
    {
        out("\033[3J\033[H\033[2J");
        m_group_stack_depth = 0;
        fflush(stdout);
    }

    virtual void end_group() override
    {
        if (m_group_stack_depth > 0)
            m_group_stack_depth--;
    }

    // 2.3. Printer(logLevel, args[, options]), https://console.spec.whatwg.org/#printer
    virtual JS::ThrowCompletionOr<JS::Value> printer(JS::Console::LogLevel log_level, PrinterArguments arguments) override
    {
        DeprecatedString indent = DeprecatedString::repeated("  "sv, m_group_stack_depth);

        if (log_level == JS::Console::LogLevel::Trace) {
            auto trace = arguments.get<JS::Console::Trace>();
            StringBuilder builder;
            if (!trace.label.is_empty())
                builder.appendff("{}\033[36;1m{}\033[0m\n", indent, trace.label);

            for (auto& function_name : trace.stack)
                builder.appendff("{}-> {}\n", indent, function_name);

            outln("{}", builder.string_view());
            return JS::js_undefined();
        }

        if (log_level == JS::Console::LogLevel::Group || log_level == JS::Console::LogLevel::GroupCollapsed) {
            auto group = arguments.get<JS::Console::Group>();
            outln("{}\033[36;1m{}\033[0m", indent, group.label);
            m_group_stack_depth++;
            return JS::js_undefined();
        }

        auto output = DeprecatedString::join(' ', arguments.get<JS::MarkedVector<JS::Value>>());
#ifdef AK_OS_SERENITY
        m_console.output_debug_message(log_level, output);
#endif

        switch (log_level) {
        case JS::Console::LogLevel::Debug:
            outln("{}\033[36;1m{}\033[0m", indent, output);
            break;
        case JS::Console::LogLevel::Error:
        case JS::Console::LogLevel::Assert:
            outln("{}\033[31;1m{}\033[0m", indent, output);
            break;
        case JS::Console::LogLevel::Info:
            outln("{}(i) {}", indent, output);
            break;
        case JS::Console::LogLevel::Log:
            outln("{}{}", indent, output);
            break;
        case JS::Console::LogLevel::Warn:
        case JS::Console::LogLevel::CountReset:
            outln("{}\033[33;1m{}\033[0m", indent, output);
            break;
        default:
            outln("{}{}", indent, output);
            break;
        }
        return JS::js_undefined();
    }

private:
    int m_group_stack_depth { 0 };
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio rpath wpath cpath tty sigaction"));

    bool gc_on_every_allocation = false;
    bool disable_syntax_highlight = false;
    StringView evaluate_script;
    Vector<StringView> script_paths;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("This is a JavaScript interpreter.");
    args_parser.add_option(s_dump_ast, "Dump the AST", "dump-ast", 'A');
    args_parser.add_option(JS::Bytecode::g_dump_bytecode, "Dump the bytecode", "dump-bytecode", 'd');
    args_parser.add_option(s_run_bytecode, "Run the bytecode", "run-bytecode", 'b');
    args_parser.add_option(s_opt_bytecode, "Optimize the bytecode", "optimize-bytecode", 'p');
    args_parser.add_option(s_as_module, "Treat as module", "as-module", 'm');
    args_parser.add_option(s_print_last_result, "Print last result", "print-last-result", 'l');
    args_parser.add_option(s_strip_ansi, "Disable ANSI colors", "disable-ansi-colors", 'i');
    args_parser.add_option(s_disable_source_location_hints, "Disable source location hints", "disable-source-location-hints", 'h');
    args_parser.add_option(gc_on_every_allocation, "GC on every allocation", "gc-on-every-allocation", 'g');
    args_parser.add_option(disable_syntax_highlight, "Disable live syntax highlighting", "no-syntax-highlight", 's');
    args_parser.add_option(evaluate_script, "Evaluate argument as a script", "evaluate", 'c', "script");
    args_parser.add_positional_argument(script_paths, "Path to script files", "scripts", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    bool syntax_highlight = !disable_syntax_highlight;

    g_vm = JS::VM::create();
    g_vm->enable_default_host_import_module_dynamically_hook();

    // NOTE: These will print out both warnings when using something like Promise.reject().catch(...) -
    // which is, as far as I can tell, correct - a promise is created, rejected without handler, and a
    // handler then attached to it. The Node.js REPL doesn't warn in this case, so it's something we
    // might want to revisit at a later point and disable warnings for promises created this way.
    g_vm->on_promise_unhandled_rejection = [](auto& promise) {
        warn("WARNING: A promise was rejected without any handlers");
        warn(" (result: ");
        (void)print(promise.result(), PrintTarget::StandardError);
        warnln(")");
    };
    g_vm->on_promise_rejection_handled = [](auto& promise) {
        warn("WARNING: A handler was added to an already rejected promise");
        warn(" (result: ");
        (void)print(promise.result(), PrintTarget::StandardError);
        warnln(")");
    };
    OwnPtr<JS::Interpreter> interpreter;

    // FIXME: Figure out some way to interrupt the interpreter now that vm.exception() is gone.

    if (evaluate_script.is_empty() && script_paths.is_empty()) {
        s_print_last_result = true;
        interpreter = JS::Interpreter::create<ReplObject>(*g_vm);
        auto& console_object = *interpreter->realm().intrinsics().console_object();
        ReplConsoleClient console_client(console_object.console());
        console_object.console().set_client(console_client);
        interpreter->heap().set_should_collect_on_every_allocation(gc_on_every_allocation);

        auto& global_environment = interpreter->realm().global_environment();

        s_editor = Line::Editor::construct();
        s_editor->load_history(s_history_path);

        signal(SIGINT, [](int) {
            if (!s_editor->is_editing())
                sigint_handler();
            s_editor->save_history(s_history_path);
        });

        s_editor->on_display_refresh = [syntax_highlight](Line::Editor& editor) {
            auto stylize = [&](Line::Span span, Line::Style styles) {
                if (syntax_highlight)
                    editor.stylize(span, styles);
            };
            editor.strip_styles();

            size_t open_indents = s_repl_line_level;

            auto line = editor.line();
            JS::Lexer lexer(line);
            bool indenters_starting_line = true;
            for (JS::Token token = lexer.next(); token.type() != JS::TokenType::Eof; token = lexer.next()) {
                auto length = Utf8View { token.value() }.length();
                auto start = token.offset();
                auto end = start + length;
                if (indenters_starting_line) {
                    if (token.type() != JS::TokenType::ParenClose && token.type() != JS::TokenType::BracketClose && token.type() != JS::TokenType::CurlyClose) {
                        indenters_starting_line = false;
                    } else {
                        --open_indents;
                    }
                }

                switch (token.category()) {
                case JS::TokenCategory::Invalid:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Red), Line::Style::Underline });
                    break;
                case JS::TokenCategory::Number:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Magenta) });
                    break;
                case JS::TokenCategory::String:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Green), Line::Style::Bold });
                    break;
                case JS::TokenCategory::Punctuation:
                    break;
                case JS::TokenCategory::Operator:
                    break;
                case JS::TokenCategory::Keyword:
                    switch (token.type()) {
                    case JS::TokenType::BoolLiteral:
                    case JS::TokenType::NullLiteral:
                        stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Yellow), Line::Style::Bold });
                        break;
                    default:
                        stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Blue), Line::Style::Bold });
                        break;
                    }
                    break;
                case JS::TokenCategory::ControlKeyword:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Cyan), Line::Style::Italic });
                    break;
                case JS::TokenCategory::Identifier:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::White), Line::Style::Bold });
                    break;
                default:
                    break;
                }
            }

            editor.set_prompt(prompt_for_level(open_indents));
        };

        auto complete = [&interpreter, &global_environment](Line::Editor const& editor) -> Vector<Line::CompletionSuggestion> {
            auto line = editor.line(editor.cursor());

            JS::Lexer lexer { line };
            enum {
                Initial,
                CompleteVariable,
                CompleteNullProperty,
                CompleteProperty,
            } mode { Initial };

            StringView variable_name;
            StringView property_name;

            // we're only going to complete either
            //    - <N>
            //        where N is part of the name of a variable
            //    - <N>.<P>
            //        where N is the complete name of a variable and
            //        P is part of the name of one of its properties
            auto js_token = lexer.next();
            for (; js_token.type() != JS::TokenType::Eof; js_token = lexer.next()) {
                switch (mode) {
                case CompleteVariable:
                    switch (js_token.type()) {
                    case JS::TokenType::Period:
                        // ...<name> <dot>
                        mode = CompleteNullProperty;
                        break;
                    default:
                        // not a dot, reset back to initial
                        mode = Initial;
                        break;
                    }
                    break;
                case CompleteNullProperty:
                    if (js_token.is_identifier_name()) {
                        // ...<name> <dot> <name>
                        mode = CompleteProperty;
                        property_name = js_token.value();
                    } else {
                        mode = Initial;
                    }
                    break;
                case CompleteProperty:
                    // something came after the property access, reset to initial
                case Initial:
                    if (js_token.type() == JS::TokenType::Identifier) {
                        // ...<name>...
                        mode = CompleteVariable;
                        variable_name = js_token.value();
                    } else {
                        mode = Initial;
                    }
                    break;
                }
            }

            bool last_token_has_trivia = js_token.trivia().length() > 0;

            if (mode == CompleteNullProperty) {
                mode = CompleteProperty;
                property_name = ""sv;
                last_token_has_trivia = false; // <name> <dot> [tab] is sensible to complete.
            }

            if (mode == Initial || last_token_has_trivia)
                return {}; // we do not know how to complete this

            Vector<Line::CompletionSuggestion> results;

            Function<void(JS::Shape const&, StringView)> list_all_properties = [&results, &list_all_properties](JS::Shape const& shape, auto property_pattern) {
                for (auto const& descriptor : shape.property_table()) {
                    if (!descriptor.key.is_string())
                        continue;
                    auto key = descriptor.key.as_string();
                    if (key.view().starts_with(property_pattern)) {
                        Line::CompletionSuggestion completion { key, Line::CompletionSuggestion::ForSearch };
                        if (!results.contains_slow(completion)) { // hide duplicates
                            results.append(DeprecatedString(key));
                            results.last().invariant_offset = property_pattern.length();
                        }
                    }
                }
                if (auto const* prototype = shape.prototype()) {
                    list_all_properties(prototype->shape(), property_pattern);
                }
            };

            switch (mode) {
            case CompleteProperty: {
                auto reference_or_error = g_vm->resolve_binding(variable_name, &global_environment);
                if (reference_or_error.is_error())
                    return {};
                auto value_or_error = reference_or_error.value().get_value(*g_vm);
                if (value_or_error.is_error())
                    return {};
                auto variable = value_or_error.value();
                VERIFY(!variable.is_empty());

                if (!variable.is_object())
                    break;

                auto const* object = MUST(variable.to_object(*g_vm));
                auto const& shape = object->shape();
                list_all_properties(shape, property_name);
                break;
            }
            case CompleteVariable: {
                auto const& variable = interpreter->realm().global_object();
                list_all_properties(variable.shape(), variable_name);

                for (auto const& name : global_environment.declarative_record().bindings()) {
                    if (name.starts_with(variable_name)) {
                        results.empend(name);
                        results.last().invariant_offset = variable_name.length();
                    }
                }

                break;
            }
            default:
                VERIFY_NOT_REACHED();
            }

            return results;
        };
        s_editor->on_tab_complete = move(complete);
        TRY(repl(*interpreter));
        s_editor->save_history(s_history_path);
    } else {
        interpreter = JS::Interpreter::create<ScriptObject>(*g_vm);
        auto& console_object = *interpreter->realm().intrinsics().console_object();
        ReplConsoleClient console_client(console_object.console());
        console_object.console().set_client(console_client);
        interpreter->heap().set_should_collect_on_every_allocation(gc_on_every_allocation);

        signal(SIGINT, [](int) {
            sigint_handler();
        });

        StringBuilder builder;
        StringView source_name;

        if (evaluate_script.is_empty()) {
            if (script_paths.size() > 1)
                warnln("Warning: Multiple files supplied, this will concatenate the sources and resolve modules as if it was the first file");

            for (auto& path : script_paths) {
                auto file = TRY(Core::Stream::File::open(path, Core::Stream::OpenMode::Read));
                auto file_contents = TRY(file->read_until_eof());
                auto source = StringView { file_contents };

                if (Utf8View { file_contents }.validate()) {
                    builder.append(source);
                } else {
                    auto* decoder = TextCodec::decoder_for("windows-1252");
                    VERIFY(decoder);

                    auto utf8_source = TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, source);
                    builder.append(utf8_source);
                }
            }

            source_name = script_paths[0];
        } else {
            builder.append(evaluate_script);
            source_name = "eval"sv;
        }

        // We resolve modules as if it is the first file

        if (!TRY(parse_and_run(*interpreter, builder.string_view(), source_name)))
            return 1;
    }

    return 0;
}
