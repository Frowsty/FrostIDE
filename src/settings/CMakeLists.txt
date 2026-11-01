# SPDX-License-Identifier: Apache-2.0
#
# frost::settings (PLAN.md section 3.6; ADR 0021): the typed settings registry with FrostIDE's
# built-in keys, scopes (application, project-shared, project-personal, EditorConfig per file) and
# their precedence, the trust rule for `executes` keys, JSON scope files with byte-exact edits,
# migrations, live reload from file-watcher batches, validation on save and the .frost/ layout.
# Qt-free; depends on io (and core and sched through it). yyjson (MIT) is linked PRIVATE, so no layer
# above settings sees yyjson.h.
# TODO(M11): importers (Rider/CLion .DotSettings and .idea, VS .vssettings, VS Code settings.json).

frost_add_library(settings LAYER settings
    SOURCES
        src/builtin_registry.cpp
        src/document.cpp
        src/editorconfig.cpp
        src/json.cpp
        src/migration.cpp
        src/project_layout.cpp
        src/registry.cpp
        src/store.cpp
        src/validation.cpp
        src/value.cpp
    DEPS frost::io)
target_link_libraries(frost_settings PRIVATE yyjson::yyjson)
