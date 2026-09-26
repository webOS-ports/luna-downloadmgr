// Copyright (c) 2013-2018 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "JUtil.h"
#include "Utils.h"
#include "Logging.h"
#include "DownloadSettings.h"

class DefaultResolver : public pbnjson::JResolver
{
public:
    pbnjson::JSchema resolve(const ResolutionRequest &request, JSchemaResolutionResult &result)
    {
        //TODO : If we use cached schema here, resolve fail. Is it pbnjson bug? or misusage?
        // uncached, and without passing ourselves back down: a schema that
        // $refs itself would otherwise recurse forever
        pbnjson::JSchema resolved = JUtil::instance().loadSchema(request.resource(), false);
        if (!resolved.isInitialized())
        {
            LOG_WARNING_PAIRS(LOGID_SCHEMA_IO_ERROR,1,PMLOGKS("ERRTEXT",request.resource().c_str()),"");
            result = SCHEMA_IO_ERROR;
            return pbnjson::JSchema::NullSchema();
        }

        result = SCHEMA_RESOLVED;
        return resolved;
    }
};

JUtil::Error::Error()
    : m_code(Error::None)
{
}

JUtil::Error::ErrorCode JUtil::Error::code()
{
    return m_code;
}

std::string JUtil::Error::detail()
{
    return m_detail;
}

void JUtil::Error::set(ErrorCode code, const char *detail)
{
    m_code = code;
    if (!detail)
    {
        switch(m_code)
        {
            case Error::None:    m_detail = "Success"; break;
            case Error::File_Io: m_detail = "Fail to read file"; break;
            case Error::Schema:  m_detail = "Fail to read schema"; break;
            case Error::Parse:   m_detail = "Fail to parse json"; break;
            default:             m_detail = "Unknown error"; break;
        }
    }
    else
        m_detail = detail;
}

JUtil::JUtil()
{
}

JUtil::~JUtil()
{
}

pbnjson::JValue JUtil::parse(const char *rawData, const std::string &schemaName, Error *error, pbnjson::JResolver *schemaResolver)
{
    if (!rawData)
    {
        if (error) error->set(Error::Parse, "no input");
        return pbnjson::JValue();
    }

    DefaultResolver resolver;
    if (!schemaResolver)
        schemaResolver = &resolver;

    // $ref links are resolved inside loadSchema() now: JDomParser(JResolver*)
    // and the JErrorHandler overload of parse() are both gone in pbnjson 3.0,
    // and JSchema::resolve() is the documented replacement.
    pbnjson::JSchema schema = JUtil::instance().loadSchema(schemaName, true, schemaResolver);
    if (!schema.isInitialized())
    {
        if (error) error->set(Error::Schema);
        return pbnjson::JValue();
    }

    pbnjson::JDomParser parser;
    if (!parser.parse(rawData, schema))
    {
        // JParser::getError() carries the message DefaultErrorHandler used to
        // assemble. It does not say whether the input was malformed or merely
        // failed validation, and callers surface Error::code() as well as
        // detail(), so re-parse without the schema to tell the two apart. Only
        // the failure path pays for that.
        const char *detail = parser.getError();
        Error::ErrorCode code = Error::Parse;
        if (!schemaName.empty())
        {
            pbnjson::JDomParser schemaless;
            if (schemaless.parse(rawData))
                code = Error::Schema;   // valid JSON, rejected by the schema
        }
        LOG_WARNING_PAIRS(LOGID_JSON_PARSE_FAIL, 2,
                          PMLOGKS("SCHEMA", schemaName.empty() ? "(none)" : schemaName.c_str()),
                          PMLOGKS("ERRTEXT", (detail && *detail) ? detail : "unspecified"), "");
        if (error) error->set(code, detail);
        return pbnjson::JValue();
    }

    if (error) error->set(Error::None);
    return parser.getDom();
}

pbnjson::JValue JUtil::parseFile(const std::string &path, const std::string &schemaName, Error *error, pbnjson::JResolver *schemaResolver)
{
    std::string rawData = Utils::read_file(path);
    if (rawData.empty())
    {
        if (error) error->set(Error::File_Io);
        return pbnjson::JValue();
    }

    pbnjson::JValue parsed = parse(rawData.c_str(), schemaName, error, schemaResolver);

    return parsed;
}

std::string JUtil::toSimpleString(pbnjson::JValue json)
{
    // AllSchema() is a shared "accept anything" schema; JSchemaFragment("{}")
    // parsed the same schema text on every single call, and its own header calls
    // it a temporary convenience class.
    return pbnjson::JGenerator::serialize(json, pbnjson::JSchema::AllSchema());
}

pbnjson::JSchema JUtil::loadSchema(const std::string& schemaName, bool cache,
                                   pbnjson::JResolver *resolver)
{
    if (schemaName.empty())
        return pbnjson::JSchema::AllSchema();

    if (cache)
    {
        std::map< std::string, pbnjson::JSchema >::iterator it = m_mapSchema.find(schemaName);
        if (it != m_mapSchema.end())
            return it->second;
    }

    const std::string path = DownloadSettings::instance().schemaPath + schemaName + ".schema";
    pbnjson::JSchema schema = pbnjson::JSchema::fromFile(path.c_str());
    if (!schema.isInitialized())
    {
        LOG_WARNING_PAIRS(LOGID_SCHEMA_IO_ERROR, 2, PMLOGKS("SCHEMA", path.c_str()),
                          PMLOGKS("ERRTEXT", schema.errorString().c_str()), "");
        return schema;
    }

    // Resolve $ref links now rather than at parse time. None of this component's
    // schemas currently use $ref, so this is normally a no-op; doing it before
    // the schema is cached means it happens once per schema instead of once per
    // request, which is the whole point of the pbnjson 3.0 change.
    if (resolver && !schema.resolve(*resolver))
    {
        LOG_WARNING_PAIRS(LOGID_SCHEMA_IO_ERROR, 2, PMLOGKS("SCHEMA", path.c_str()),
                          PMLOGKS("ERRTEXT", "failed to resolve external references"), "");
        return pbnjson::JSchema::NullSchema();
    }

    if (cache)
    {
        m_mapSchema.insert( std::pair< std::string, pbnjson::JSchema >(schemaName, schema) );
    }

    return schema;
}
