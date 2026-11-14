// SPDX-License-Identifier: Apache-2.0
// AST fixture: deliberately different Lua/C++ names, actual overloads/aliases/lambdas and ambiguous sources.

namespace Frost {
struct ScriptDoc {
    const char* Signature = "";
    const char* Description = "";
};
struct SourceLocation {};
enum class ScriptOperator { Add };
struct ScriptNamespaceBuilder {
    template<class F>
    ScriptNamespaceBuilder& Function(const char*, F&&, ScriptDoc, SourceLocation = {}) {
        return *this;
    }
    template<class Getter>
    ScriptNamespaceBuilder& Property(const char*, Getter&&, ScriptDoc, SourceLocation = {}) {
        return *this;
    }
    template<class Getter, class Setter>
    ScriptNamespaceBuilder& Property(const char*, Getter&&, Setter&&, ScriptDoc, SourceLocation = {}) {
        return *this;
    }
};
struct ScriptApiBuilder {
    template<class F>
    void Function(const char*, F&&, ScriptDoc, SourceLocation = {}) {}
};
struct ScriptTypeBuilder {
    template<class F>
    ScriptTypeBuilder& StaticFunction(const char*, F&&, ScriptDoc, SourceLocation = {}) {
        return *this;
    }
    template<class F>
    ScriptTypeBuilder& Constructor(F&&, ScriptDoc, SourceLocation = {}) {
        return *this;
    }
    template<class F>
    ScriptTypeBuilder& Operator(ScriptOperator, F&&, ScriptDoc, SourceLocation = {}) {
        return *this;
    }
};
} // namespace Frost

namespace sol {
template<class... F>
struct overload_set {};
template<class... F>
overload_set<F...> overload(F...) {
    return {};
}
} // namespace sol

namespace native {
int declaredTarget(int);
int declaredTarget(int input) { return input + 5; }
int actualTarget(int input) { return input + 7; }
int anotherTarget(int input) { return input - 3; }
int visibleLuaName(int input) { return input * 99; } // Equal spelling grants no authority.
int overloaded(int input) { return input + 1; }
double overloaded(double input) { return input + 0.5; }
int getter() { return 1; }
void setter(int) {}
struct Object {
    int method(int input) { return input; }
    int field;
};
struct CallbackOwner {
    int (*callback)(int) = &actualTarget;
};
} // namespace native

constexpr auto constantAlias = &native::actualTarget;
constexpr auto aliasOfAlias = constantAlias;
auto mutableAlias = &native::actualTarget;
auto factory() { return &native::actualTarget; }
#define NATIVE_ARGUMENT &native::actualTarget
#define LUA_NAME "macroName"

struct ForeignBuilder {
    template<class F>
    void Function(const char*, F&&, Frost::ScriptDoc) {}
};

void registrations(Frost::ScriptNamespaceBuilder& api, Frost::ScriptApiBuilder& global, Frost::ScriptTypeBuilder& type,
                   bool choose, ForeignBuilder& foreign) {
    native::CallbackOwner object;
    const char* computedName = choose ? "computed" : "other";
    const char* computedSignature = choose ? "computedDoc(): integer" : "other(): integer";
    Frost::ScriptDoc mutableDoc{.Signature = "mutableDoc(): integer"};
    // clang-format off: the actual registration-site line is part of this AST fixture.
    api.Function("documented", &native::actualTarget, {.Signature = "  documented(): integer", .Description = "decoy(): string"}); // BIND documented
    api.Function("docEscaped", &native::actualTarget, {.Signature = "\x64ocEscaped(): integer"}); // BIND docEscaped
    api.Function("computedDoc", &native::actualTarget, {.Signature = computedSignature}); // BIND computedDoc
    api.Function("mutableDoc", &native::actualTarget, mutableDoc); // BIND mutableDoc
    api.Function("mismatchedDoc", &native::actualTarget, {.Signature = "other(): integer"}); // BIND mismatchedDoc
    // clang-format on
    api.Function( // BIND multiline
        "multiline", &native::actualTarget, {});
    api.Function("visibleLuaName", &native::actualTarget, {}); // BIND direct
    api.Function("alias", aliasOfAlias, {});                   // BIND alias
    api.Function("declared", &native::declaredTarget, {});     // BIND declared
    api.Function("esc\x61ped", &native::actualTarget, {});     // BIND escaped
    // clang-format off: schema source line identifies the actual member call, not its closing argument.
    api.Function("concat" "enated", &native::actualTarget, {}); // BIND concatenated
    // clang-format on
    api.Function(computedName, &native::actualTarget, {});                                         // BIND computed
    api.Function(LUA_NAME, &native::actualTarget, {});                                             // BIND macroName
    api.Function("integer", static_cast<int (*)(int)>(&native::overloaded), {});                   // BIND integer
    api.Function("real", static_cast<double (*)(double)>(&native::overloaded), {});                // BIND real
    api.Function("lambda", [](int input) { return native::actualTarget(input); }, {});             // BIND lambda
    api.Function("lambdaTwo", [](int input) { return native::actualTarget(input); }, {});          // BIND lambdaTwo
    api.Function("mutable", mutableAlias, {});                                                     // BIND mutable
    api.Function("mutableMember", object.callback, {});                                            // BIND mutableMember
    api.Function("factory", factory(), {});                                                        // BIND factory
    api.Function("macro", NATIVE_ARGUMENT, {});                                                    // BIND macro
    api.Function("conditional", choose ? &native::actualTarget : &native::anotherTarget, {});      // BIND conditional
    api.Function("overloadSet", sol::overload(&native::actualTarget, &native::anotherTarget), {}); // BIND overloadSet
    api.Property("property", &native::getter, &native::setter, {});                                // BIND property
    api.Property("field", &native::Object::field, {});                                             // BIND field
    api.Function("member", &native::Object::method, {});                                           // BIND member
    global.Function("global", &native::actualTarget, {});                                          // BIND global
    type.StaticFunction("static", &native::actualTarget, {});                                      // BIND static
    type.Constructor([](int input) { return native::Object{input}; }, {});                         // BIND constructor
    type.Operator(Frost::ScriptOperator::Add, &native::actualTarget, {});                          // BIND operator
    // clang-format off: same physical source line is the ambiguity under test.
    api.Function("duplicate", &native::actualTarget, {}); api.Function("duplicate", &native::anotherTarget, {}); // BIND duplicate
    // clang-format on
    foreign.Function("foreign", &native::actualTarget, {}); // BIND foreign
    // api.Function("comment", &native::actualTarget, {}); // BIND comment
    const char* text = "api.Function(\"string\", &native::actualTarget, {});"; // BIND string
    (void)text;
}
