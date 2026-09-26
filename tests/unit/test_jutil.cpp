// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// JUtil is the schema-validation front door: every luna method's payload goes
// through JUtil::parse() with the method's .schema file, and the Error it fills
// in becomes the errorText in the reply. These cases pin that contract, because
// the migration off pbnjson's deprecated APIs replaced the JErrorHandler that
// used to classify failures with JParser::getError().

#include <gtest/gtest.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>

#include "DownloadSettings.h"
#include "JUtil.h"

namespace {

// Point the schema loader at a directory we control, so these tests do not
// depend on the component being installed.
class JUtilSchemaEnv : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_savedPath = DownloadSettings::instance().schemaPath;
        m_dir = "ldm_test_schemas/";
        removeDir();
        ASSERT_EQ(0, ::mkdir(m_dir.c_str(), 0755));
        DownloadSettings::instance().schemaPath = m_dir;
        write("Tiny", R"({
            "id": "Tiny", "type": "object",
            "properties": {
                "target":  { "type": "string" },
                "ticket":  { "type": "integer" },
                "flag":    { "type": "boolean" }
            },
            "required": [ "target" ]
        })");
        write("Broken", "{ this is not valid json at all ");
    }

    void TearDown() override
    {
        DownloadSettings::instance().schemaPath = m_savedPath;
        removeDir();
    }

    // shallow rmdir -r; the directory only ever holds the schema files below
    void removeDir()
    {
        DIR *d = ::opendir(m_dir.c_str());
        if (d) {
            struct dirent *e;
            while ((e = ::readdir(d)) != NULL) {
                std::string name(e->d_name);
                if (name == "." || name == "..")
                    continue;
                (void) ::unlink((m_dir + name).c_str());
            }
            ::closedir(d);
        }
        (void) ::rmdir(m_dir.c_str());
    }

    void write(const std::string &name, const std::string &body)
    {
        std::ofstream f((m_dir + name + ".schema").c_str());
        f << body;
    }

    std::string m_dir;
    std::string m_savedPath;
};

// ---- schemaless parsing (the empty-schemaName path) -------------------------

TEST(JUtilParse, SchemalessAcceptsAnyValidJson)
{
    JUtil::Error err;
    pbnjson::JValue v = JUtil::parse("{\"a\":1,\"b\":[1,2],\"c\":{\"d\":null}}",
                                     std::string(""), &err);
    ASSERT_FALSE(v.isNull());
    EXPECT_EQ(JUtil::Error::None, err.code());
    EXPECT_EQ(1, v["a"].asNumber<int>());
    EXPECT_EQ(2, v["b"].arraySize());
}

TEST(JUtilParse, SchemalessRejectsMalformedJsonAsAParseError)
{
    const char *bad[] = { "", "{", "}", "not json", "{\"a\":}", "[1,2", "\xff\xfe" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        JUtil::Error err;
        pbnjson::JValue v = JUtil::parse(bad[i], std::string(""), &err);
        EXPECT_TRUE(v.isNull()) << "accepted: " << bad[i];
        EXPECT_EQ(JUtil::Error::Parse, err.code()) << "input: " << bad[i];
        EXPECT_FALSE(err.detail().empty()) << "no detail for: " << bad[i];
    }
}

TEST(JUtilParse, NullInputIsAParseErrorRatherThanACrash)
{
    JUtil::Error err;
    EXPECT_TRUE(JUtil::parse(NULL, std::string(""), &err).isNull());
    EXPECT_EQ(JUtil::Error::Parse, err.code());
}

TEST(JUtilParse, ANullErrorPointerIsAllowed)
{
    EXPECT_TRUE(JUtil::parse("garbage", std::string("")).isNull());
    EXPECT_FALSE(JUtil::parse("{}", std::string("")).isNull());
}

// ---- schema validation ------------------------------------------------------

TEST_F(JUtilSchemaEnv, PayloadSatisfyingTheSchemaParses)
{
    JUtil::Error err;
    pbnjson::JValue v = JUtil::parse("{\"target\":\"http://x/y\",\"ticket\":3,\"flag\":true}",
                                     std::string("Tiny"), &err);
    ASSERT_FALSE(v.isNull()) << err.detail();
    EXPECT_EQ(JUtil::Error::None, err.code());
    EXPECT_EQ("http://x/y", v["target"].asString());
}

TEST_F(JUtilSchemaEnv, MissingRequiredPropertyIsASchemaError)
{
    // Valid JSON, rejected by the schema. The old JErrorHandler reported this as
    // Error::Schema and the reply's errorText said so; that has to survive.
    JUtil::Error err;
    pbnjson::JValue v = JUtil::parse("{\"ticket\":3}", std::string("Tiny"), &err);
    EXPECT_TRUE(v.isNull());
    EXPECT_EQ(JUtil::Error::Schema, err.code());
    EXPECT_FALSE(err.detail().empty());
}

TEST_F(JUtilSchemaEnv, WrongPropertyTypeIsASchemaError)
{
    JUtil::Error err;
    pbnjson::JValue v = JUtil::parse("{\"target\":42}", std::string("Tiny"), &err);
    EXPECT_TRUE(v.isNull());
    EXPECT_EQ(JUtil::Error::Schema, err.code());
}

TEST_F(JUtilSchemaEnv, MalformedJsonIsAParseErrorEvenWithASchema)
{
    // The distinction between "not JSON" and "JSON the schema refuses" is what
    // the re-parse in JUtil::parse() exists to preserve.
    JUtil::Error err;
    pbnjson::JValue v = JUtil::parse("{\"target\":", std::string("Tiny"), &err);
    EXPECT_TRUE(v.isNull());
    EXPECT_EQ(JUtil::Error::Parse, err.code());
}

TEST_F(JUtilSchemaEnv, AMissingSchemaFileIsASchemaError)
{
    JUtil::Error err;
    pbnjson::JValue v = JUtil::parse("{\"target\":\"x\"}", std::string("NoSuchSchema"), &err);
    EXPECT_TRUE(v.isNull());
    EXPECT_EQ(JUtil::Error::Schema, err.code());
}

TEST_F(JUtilSchemaEnv, AnUnparseableSchemaFileIsASchemaError)
{
    JUtil::Error err;
    pbnjson::JValue v = JUtil::parse("{\"target\":\"x\"}", std::string("Broken"), &err);
    EXPECT_TRUE(v.isNull());
    EXPECT_EQ(JUtil::Error::Schema, err.code());
}

TEST_F(JUtilSchemaEnv, SchemasAreCachedButStillValidate)
{
    // loadSchema() resolves and caches; a cached schema has to keep rejecting
    // what it rejected the first time.
    for (int i = 0; i < 3; ++i) {
        JUtil::Error ok, bad;
        EXPECT_FALSE(JUtil::parse("{\"target\":\"x\"}", std::string("Tiny"), &ok).isNull());
        EXPECT_EQ(JUtil::Error::None, ok.code());
        EXPECT_TRUE(JUtil::parse("{}", std::string("Tiny"), &bad).isNull());
        EXPECT_EQ(JUtil::Error::Schema, bad.code());
    }
}

TEST_F(JUtilSchemaEnv, LoadSchemaWithAnEmptyNameAcceptsEverything)
{
    pbnjson::JSchema all = JUtil::instance().loadSchema(std::string(""), false);
    EXPECT_TRUE(all.isInitialized());

    pbnjson::JDomParser parser;
    EXPECT_TRUE(parser.parse("{\"anything\":[1,\"two\",null]}", all));
}

// ---- serialization ----------------------------------------------------------

TEST(JUtilSerialize, RoundTripsThroughToSimpleString)
{
    pbnjson::JValue o = pbnjson::Object();
    o.put("s", "text");
    o.put("n", (int64_t) 6000000000ll);
    o.put("b", true);

    std::string out = JUtil::toSimpleString(o);
    ASSERT_FALSE(out.empty());

    pbnjson::JValue back = JUtil::parse(out.c_str(), std::string(""));
    ASSERT_FALSE(back.isNull()) << out;
    EXPECT_EQ("text", back["s"].asString());
    EXPECT_EQ(6000000000ll, back["n"].asNumber<int64_t>());
    EXPECT_TRUE(back["b"].asBool());
}

TEST(JUtilSerialize, EscapesMetacharactersSoTheResultReparses)
{
    // Reply payloads carry caller-supplied URLs and filenames.
    const char *nasty[] = {
        "a\"b", "a\\b", "line\nbreak", "tab\there", "nul-free \x01\x02",
        "caf\xc3\xa9", "}{\"injected\":true}",
    };
    for (size_t i = 0; i < sizeof(nasty) / sizeof(nasty[0]); ++i) {
        pbnjson::JValue o = pbnjson::Object();
        o.put("v", nasty[i]);
        std::string out = JUtil::toSimpleString(o);
        pbnjson::JValue back = JUtil::parse(out.c_str(), std::string(""));
        ASSERT_FALSE(back.isNull()) << "did not reparse: " << out;
        EXPECT_EQ(std::string(nasty[i]), back["v"].asString());
    }
}

TEST(JUtilSerialize, IteratingChildrenSeesEveryProperty)
{
    // children() replaced the deprecated begin()/end(); cbDownloadStatusQuery
    // copies a whole stored record into its reply this way, so nothing may be
    // dropped.
    pbnjson::JValue o = pbnjson::Object();
    o.put("one", 1);
    o.put("two", "2");
    o.put("three", true);
    o.put("four", pbnjson::Array());

    int seen = 0;
    pbnjson::JValue copy = pbnjson::Object();
    for (const pbnjson::JValue::KeyValue &kv : o.children()) {
        copy.put(kv.first.asString(), kv.second);
        ++seen;
    }
    EXPECT_EQ(4, seen);
    EXPECT_EQ(1, copy["one"].asNumber<int>());
    EXPECT_EQ("2", copy["two"].asString());
    EXPECT_TRUE(copy["three"].asBool());
    EXPECT_TRUE(copy["four"].isArray());
}

TEST(JUtilSerialize, IteratingAnEmptyObjectYieldsNothing)
{
    pbnjson::JValue empty = pbnjson::Object();
    int seen = 0;
    for (const pbnjson::JValue::KeyValue &kv : empty.children()) { (void) kv; ++seen; }
    EXPECT_EQ(0, seen);
}

}   // namespace
