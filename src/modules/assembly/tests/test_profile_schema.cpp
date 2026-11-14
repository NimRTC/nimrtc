// =============================================================================
// test_profile_schema.cpp — Profile JSON schema conformance tests (v1.2)
// =============================================================================
//
// ## What this test verifies
//
// K4 of docs/plan/remote-desktop-plan.md: `profiles/schema/profile-v1.2.json`
// is **strictly additive** on v1.1.  This test asserts that:
//
//   1. Every profile JSON shipped under `profiles/` validates against
//      `profiles/schema/profile-v1.2.json` — including the v1.1 profiles
//      that do not declare any of the four new video-scene keys.
//   2. The four v1.2 keys are **optional** — they are not in the schema's
//      `required` list, and a v1.1 profile validates unchanged.
//   3. An *invalid* value for a v1.2 key (e.g. `video_ladder_mode:
//      "bogus"`) is rejected by the schema.
//
// ## Why a hand-rolled validator
//
// The repository vendors nlohmann_json (a JSON *parser*), not a JSON
// Schema *validator* — there is no `jsonschema` implementation anywhere in
// the tree (grep for "jsonschema" / "json_schema" over `tests/` and
// `src/modules/assembly/tests/` finds nothing).  Rather than add a
// dependency for a check that is a couple of hundred lines, the test
// implements the small subset of JSON Schema draft 2020-12 that the
// Profile schemas actually use:
//
//   type (string / integer / number / boolean / object / array)
//   enum
//   required
//   properties (recursive)
//   additionalProperties: false  (or a schema every unmatched member obeys)
//   propertyNames (a schema every member's *name* obeys)
//   minimum / maximum
//   minLength / maxLength
//
// Anything the validator does not understand is ignored rather than
// silently treated as valid-by-default for the *check* itself: the
// `properties` map is walked exhaustively, so a new key added to a
// shipped profile that the schema does not mention is caught when the
// schema says `additionalProperties: false`.
//
// ## Paths
//
// The test reads the profiles and the schema from the **source tree**,
// located via the `NIMRTC_SOURCE_ROOT` compile-time injection (the same
// mechanism `tests/test_api_classification.cpp` uses).  Unlike the
// runtime-copied `profiles/` directory, the schema lives in a subdirectory
// that the assembly module's copy step does not mirror.
//
// The profile JSONs contain `//` comments, so the parser is invoked with
// `ignore_comments = true` — the same way the production loader
// (`assembly::profile_from_json_file`) parses them.
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef NIMRTC_SOURCE_ROOT
    #error "NIMRTC_SOURCE_ROOT must be defined via CMake target_compile_definitions"
#endif

namespace fs = std::filesystem;

namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Schema model — the subset of JSON Schema the Profile schemas use.
// ---------------------------------------------------------------------------
struct Schema {
    json root;
};

// Load a JSON document, tolerating `//` and `/* */` comments (the shipped
// profile files use them extensively).
json load_json(const fs::path& path) {
    std::ifstream file{path};
    EXPECT_TRUE(file.is_open()) << "Cannot open: " << path.string();
    json out = json::parse(file, /*cb=*/nullptr, /*allow_exceptions=*/true,
                           /*ignore_comments=*/true);
    return out;
}

// True if `value` satisfies the schema's `type` keyword.
bool type_matches(const std::string& type, const json& value) {
    if (type == "object")  return value.is_object();
    if (type == "array")   return value.is_array();
    if (type == "string")  return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "integer") return value.is_number_integer();
    if (type == "number")  return value.is_number();
    // An unknown type keyword is not a constraint we can check.
    return true;
}

// Collect the validation errors of `value` against `schema_node`.
// `path` is the JSON pointer-ish location used in the error messages.
std::vector<std::string> validate_against(const json& schema_node,
                                          const json& value,
                                          const std::string& path) {
    std::vector<std::string> errors;

    // -- type ----------------------------------------------------------------
    if (schema_node.contains("type") && schema_node["type"].is_string()) {
        const std::string type = schema_node["type"].get<std::string>();
        if (!type_matches(type, value)) {
            errors.push_back(path + ": expected type '" + type + "', got " +
                             std::string{value.type_name()});
            return errors;  // further keywords assume the right type
        }
    }

    // -- enum ----------------------------------------------------------------
    if (schema_node.contains("enum") && schema_node["enum"].is_array()) {
        bool found = false;
        for (const auto& allowed : schema_node["enum"]) {
            if (allowed == value) { found = true; break; }
        }
        if (!found) {
            std::string allowed_list;
            for (const auto& allowed : schema_node["enum"]) {
                if (!allowed_list.empty()) allowed_list += ", ";
                allowed_list += allowed.dump();
            }
            errors.push_back(path + ": value " + value.dump() +
                             " is not one of [" + allowed_list + "]");
        }
    }

    // -- string bounds -------------------------------------------------------
    if (value.is_string()) {
        const std::string str = value.get<std::string>();
        if (schema_node.contains("minLength") &&
            str.size() < schema_node["minLength"].get<std::size_t>()) {
            errors.push_back(path + ": string length " +
                             std::to_string(str.size()) + " < minLength " +
                             schema_node["minLength"].dump());
        }
        if (schema_node.contains("maxLength") &&
            str.size() > schema_node["maxLength"].get<std::size_t>()) {
            errors.push_back(path + ": string length " +
                             std::to_string(str.size()) + " > maxLength " +
                             schema_node["maxLength"].dump());
        }
    }

    // -- numeric bounds ------------------------------------------------------
    if (value.is_number()) {
        if (schema_node.contains("minimum") &&
            value.get<double>() < schema_node["minimum"].get<double>()) {
            errors.push_back(path + ": value " + value.dump() + " < minimum " +
                             schema_node["minimum"].dump());
        }
        if (schema_node.contains("maximum") &&
            value.get<double>() > schema_node["maximum"].get<double>()) {
            errors.push_back(path + ": value " + value.dump() + " > maximum " +
                             schema_node["maximum"].dump());
        }
    }

    // -- object --------------------------------------------------------------
    if (value.is_object()) {
        // required
        if (schema_node.contains("required") && schema_node["required"].is_array()) {
            for (const auto& key : schema_node["required"]) {
                if (!value.contains(key.get<std::string>())) {
                    errors.push_back(path + ": missing required property '" +
                                     key.get<std::string>() + "'");
                }
            }
        }

        const json props = schema_node.contains("properties")
                               ? schema_node["properties"]
                               : json::object();

        // `propertyNames` constrains the *names* of the members.  The
        // Profile schemas use it to reject an empty stream id.
        const bool has_property_names =
            schema_node.contains("propertyNames") &&
            schema_node["propertyNames"].is_object();

        // `additionalProperties` is either a boolean (the "no other
        // property" rule) or a *schema* every unmatched member must
        // satisfy.  The schema form is what a map-valued property needs
        // (the declared per-stream weights are a map of integers).
        const bool has_additional_schema =
            schema_node.contains("additionalProperties") &&
            schema_node["additionalProperties"].is_object();

        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string key = it.key();

            if (has_property_names) {
                const auto sub = validate_against(schema_node["propertyNames"],
                                                  json(key),
                                                  path + "." + key);
                errors.insert(errors.end(), sub.begin(), sub.end());
            }

            if (props.contains(key)) {
                const auto sub = validate_against(props[key], it.value(),
                                                  path + "." + key);
                errors.insert(errors.end(), sub.begin(), sub.end());
            } else if (has_additional_schema) {
                const auto sub =
                    validate_against(schema_node["additionalProperties"],
                                     it.value(), path + "." + key);
                errors.insert(errors.end(), sub.begin(), sub.end());
            } else if (schema_node.contains("additionalProperties") &&
                       schema_node["additionalProperties"].is_boolean() &&
                       !schema_node["additionalProperties"].get<bool>()) {
                errors.push_back(path + ": additional property '" + key +
                                 "' is not allowed");
            }
        }
    }

    return errors;
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Every profile JSON shipped under `profiles/` (the schema directory is
// skipped — it holds the schemas, not profiles).
std::vector<fs::path> shipped_profile_files() {
    const fs::path profiles_dir = fs::path{NIMRTC_SOURCE_ROOT} / "profiles";
    std::vector<fs::path> out;
    for (const auto& entry : fs::directory_iterator{profiles_dir}) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

fs::path schema_path(const std::string& version) {
    return fs::path{NIMRTC_SOURCE_ROOT} / "profiles" / "schema" /
           ("profile-" + version + ".json");
}

}  // namespace

// =============================================================================
// The schema itself
// =============================================================================

TEST(ProfileSchema, V12SchemaExistsAndIsNamedV12) {
    const fs::path path = schema_path("v1.2");
    ASSERT_TRUE(fs::exists(path)) << "Missing schema: " << path.string();

    const json schema = load_json(path);
    EXPECT_EQ(schema["$id"].get<std::string>(),
              "https://nimrtc.org/schema/profile-v1.2.json");
    EXPECT_EQ(schema["title"].get<std::string>(), "NimRTC Profile Schema v1.2");
}

TEST(ProfileSchema, V12AddsTheFourVideoSceneProperties) {
    const json schema = load_json(schema_path("v1.2"));
    const json props = schema["properties"];

    for (const char* key : {"video_codec_preference", "video_resolution_floor",
                            "video_ladder_mode", "video_content_type"}) {
        ASSERT_TRUE(props.contains(key))
            << "v1.2 schema must declare the property '" << key << "'";
        EXPECT_EQ(props[key]["type"].get<std::string>(), "string");
        ASSERT_TRUE(props[key].contains("enum"))
            << "'" << key << "' must carry an enum";
    }

    // The enum vocabularies.
    EXPECT_EQ(props["video_codec_preference"]["enum"],
              json({"h264", "h265", "av1", "vp8", "vp9"}));
    EXPECT_EQ(props["video_resolution_floor"]["enum"],
              json({"360p", "480p", "720p", "1080p", "1440p", "2160p"}));
    EXPECT_EQ(props["video_ladder_mode"]["enum"], json({"quality", "smoothness"}));
    EXPECT_EQ(props["video_content_type"]["enum"], json({"camera", "screen"}));
}

TEST(ProfileSchema, V12NewKeysAreOptional) {
    // The four new keys must NOT be in the `required` list — that is what
    // makes the change strictly additive (every v1.1 profile validates
    // unchanged).
    const json schema = load_json(schema_path("v1.2"));
    const json required = schema["required"];

    for (const char* key : {"video_codec_preference", "video_resolution_floor",
                            "video_ladder_mode", "video_content_type"}) {
        EXPECT_FALSE(std::find(required.begin(), required.end(), key) !=
                     required.end())
            << "'" << key << "' must be optional (not in `required`)";
    }

    // `name` stays the only required key.
    EXPECT_NE(std::find(required.begin(), required.end(), "name"), required.end());
}

TEST(ProfileSchema, V12IsAdditiveOnV11) {
    // Every property the v1.1 schema declares must still be declared —
    // with the same type — under v1.2.
    const json v11 = load_json(schema_path("v1.1"));
    const json v12 = load_json(schema_path("v1.2"));

    for (auto it = v11["properties"].begin(); it != v11["properties"].end(); ++it) {
        const std::string key = it.key();
        ASSERT_TRUE(v12["properties"].contains(key))
            << "v1.2 dropped the v1.1 property '" << key << "'";
        if (it.value().contains("type") && it.value()["type"].is_string()) {
            EXPECT_EQ(v12["properties"][key]["type"].get<std::string>(),
                      it.value()["type"].get<std::string>())
                << "v1.2 changed the type of '" << key << "'";
        }
    }
}

// =============================================================================
// Every shipped profile validates under v1.2
// =============================================================================

TEST(ProfileSchema, EveryShippedProfileValidatesUnderV12) {
    const json schema = load_json(schema_path("v1.2"));

    const auto files = shipped_profile_files();
    ASSERT_FALSE(files.empty()) << "No profile JSON found under profiles/";

    for (const auto& file : files) {
        SCOPED_TRACE("Profile: " + file.filename().string());
        const json profile = load_json(file);
        const auto errors = validate_against(schema, profile, "$");
        for (const auto& e : errors) {
            ADD_FAILURE() << file.filename().string() << ": " << e;
        }
        EXPECT_TRUE(errors.empty())
            << "Profile " << file.filename().string()
            << " does not validate under profile-v1.2.json";
    }
}

TEST(ProfileSchema, V11ProfileCallValidatesUnderV12) {
    // The explicit backward-compatibility assertion: the shipped v1.1
    // profile `call.json` — which declares none of the four v1.2 keys —
    // is valid under the v1.2 schema.
    const fs::path call_json =
        fs::path{NIMRTC_SOURCE_ROOT} / "profiles" / "call.json";
    ASSERT_TRUE(fs::exists(call_json));

    const json schema = load_json(schema_path("v1.2"));
    const json profile = load_json(call_json);

    // Sanity: the file really is a v1.1 profile.
    EXPECT_FALSE(profile.contains("video_ladder_mode"));
    EXPECT_FALSE(profile.contains("video_content_type"));
    EXPECT_FALSE(profile.contains("video_resolution_floor"));
    EXPECT_FALSE(profile.contains("video_codec_preference"));

    const auto errors = validate_against(schema, profile, "$");
    for (const auto& e : errors) {
        ADD_FAILURE() << "call.json: " << e;
    }
    EXPECT_TRUE(errors.empty())
        << "call.json (a v1.1 profile) must validate unchanged under the v1.2 schema";
}

TEST(ProfileSchema, RemoteDesktopProfileValidatesUnderV12) {
    const fs::path rd_json =
        fs::path{NIMRTC_SOURCE_ROOT} / "profiles" / "remote-desktop.json";
    ASSERT_TRUE(fs::exists(rd_json));

    const json schema = load_json(schema_path("v1.2"));
    const json profile = load_json(rd_json);

    const auto errors = validate_against(schema, profile, "$");
    for (const auto& e : errors) {
        ADD_FAILURE() << "remote-desktop.json: " << e;
    }
    EXPECT_TRUE(errors.empty());

    // And the four v1.2 keys carry the values the scene prescribes.
    EXPECT_EQ(profile["video_codec_preference"].get<std::string>(), "h264");
    EXPECT_EQ(profile["video_resolution_floor"].get<std::string>(), "720p");
    EXPECT_EQ(profile["video_ladder_mode"].get<std::string>(), "smoothness");
    EXPECT_EQ(profile["video_content_type"].get<std::string>(), "screen");
}

// =============================================================================
// The schema rejects invalid values
// =============================================================================

TEST(ProfileSchema, InvalidLadderModeIsRejected) {
    // A v1.2 profile whose ladder mode is not in the enum must NOT
    // validate — otherwise the enum is decorative.
    const json schema = load_json(schema_path("v1.2"));

    json profile = json::object();
    profile["name"] = "bogus_ladder";
    profile["video_ladder_mode"] = "bogus";

    const auto errors = validate_against(schema, profile, "$");
    EXPECT_FALSE(errors.empty())
        << "video_ladder_mode = \"bogus\" must be rejected by the v1.2 schema";
}

TEST(ProfileSchema, InvalidVideoSceneValuesAreRejected) {
    const json schema = load_json(schema_path("v1.2"));

    struct Case {
        const char* key;
        const char* value;
    };
    const Case cases[] = {
        {"video_codec_preference", "theora"},
        {"video_resolution_floor", "480i"},
        {"video_ladder_mode", "bogus"},
        {"video_content_type", "hologram"},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(std::string{"key="} + c.key + " value=" + c.value);
        json profile = json::object();
        profile["name"] = "invalid_video_scene";
        profile[c.key] = c.value;

        const auto errors = validate_against(schema, profile, "$");
        EXPECT_FALSE(errors.empty())
            << c.key << " = \"" << c.value << "\" must be rejected";
    }
}

TEST(ProfileSchema, MissingRequiredNameIsRejected) {
    // The `required` list is enforced: a profile without a name is not a
    // profile.
    const json schema = load_json(schema_path("v1.2"));

    json profile = json::object();
    profile["transport_name"] = "ice";

    const auto errors = validate_against(schema, profile, "$");
    EXPECT_FALSE(errors.empty()) << "A profile without 'name' must be rejected";
}

TEST(ProfileSchema, V12DeclaresTheStreamWeightMap) {
    // D5 — the declared per-stream bandwidth shares.  The v1.2 schema
    // declares the map: its values are non-negative integers, and its
    // keys are non-empty stream ids.
    const json schema = load_json(schema_path("v1.2"));
    const json props = schema["properties"];

    ASSERT_TRUE(props.contains("video_stream_weights"))
        << "the v1.2 schema must declare the 'video_stream_weights' property";
    EXPECT_EQ(props["video_stream_weights"]["type"].get<std::string>(), "object");

    const json values = props["video_stream_weights"]["additionalProperties"];
    ASSERT_TRUE(values.is_object())
        << "'video_stream_weights' must constrain the *values* of the map";
    EXPECT_EQ(values["type"].get<std::string>(), "integer");
    EXPECT_EQ(values["minimum"].get<int>(), 0);
    EXPECT_EQ(values["maximum"].get<int>(), 255);
}

TEST(ProfileSchema, TheStreamWeightMapIsOptional) {
    const json schema = load_json(schema_path("v1.2"));
    const json required = schema["required"];

    EXPECT_FALSE(std::find(required.begin(), required.end(),
                           "video_stream_weights") != required.end())
        << "'video_stream_weights' must be optional (not in `required`)";
}

TEST(ProfileSchema, AValidStreamWeightMapValidates) {
    const json schema = load_json(schema_path("v1.2"));

    json profile = json::object();
    profile["name"] = "weights_ok";
    profile["video_stream_weights"] = json::object();
    profile["video_stream_weights"]["default"] = 3;
    profile["video_stream_weights"]["camera"] = 1;
    profile["video_stream_weights"]["thumb"] = 0;

    const auto errors = validate_against(schema, profile, "$");
    for (const auto& e : errors) {
        ADD_FAILURE() << e;
    }
    EXPECT_TRUE(errors.empty());
}

TEST(ProfileSchema, AnInvalidStreamWeightIsRejected) {
    const json schema = load_json(schema_path("v1.2"));

    struct Case {
        const char* what;
        json value;
    };
    const Case cases[] = {
        {"a negative weight", json(-1)},
        {"a non-integer weight", json("three")},
        {"a fractional weight", json(1.5)},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(std::string{"case="} + c.what);
        json profile = json::object();
        profile["name"] = "weights_bad";
        profile["video_stream_weights"] = json::object();
        profile["video_stream_weights"]["camera"] = c.value;

        const auto errors = validate_against(schema, profile, "$");
        EXPECT_FALSE(errors.empty())
            << "a stream weight that is " << c.what
            << " must be rejected by the v1.2 schema";
    }
}

TEST(ProfileSchema, AnEmptyStreamIdIsRejected) {
    const json schema = load_json(schema_path("v1.2"));

    json profile = json::object();
    profile["name"] = "weights_empty_id";
    profile["video_stream_weights"] = json::object();
    profile["video_stream_weights"][""] = 3;

    const auto errors = validate_against(schema, profile, "$");
    EXPECT_FALSE(errors.empty())
        << "an empty stream id must be rejected by the v1.2 schema";
}

TEST(ProfileSchema, TheRemoteDesktopProfileDeclaresItsShares) {
    const fs::path rd_json =
        fs::path{NIMRTC_SOURCE_ROOT} / "profiles" / "remote-desktop.json";
    ASSERT_TRUE(fs::exists(rd_json));

    const json profile = load_json(rd_json);
    ASSERT_TRUE(profile.contains("video_stream_weights"));
    EXPECT_EQ(profile["video_stream_weights"]["default"].get<int>(), 3);
    EXPECT_EQ(profile["video_stream_weights"]["camera"].get<int>(), 1);
}
