/**
 * @file libcomp/src/DatabaseCassandra.h
 * @ingroup libcomp
 *
 * @author COMP Omega <compomega@tutanota.com>
 *
 * @brief Class to handle a Cassandra database.
 *
 * This file is part of the COMP_hack Library (libcomp).
 *
 * Copyright (C) 2012-2016 COMP_hack Team <compomega@tutanota.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "DatabaseCassandra.h"

 // libcomp Includes
#include "DatabaseQueryCassandra.h"
#include "Log.h"
#include "PersistentObject.h"

// SQLite3 Includes
#include <sqlite3.h>

using namespace libcomp;

DatabaseCassandra::DatabaseCassandra(const String& keyspace)
    : mCluster(nullptr), mSession(nullptr), mKeyspace(keyspace.ToUtf8())
{
}

DatabaseCassandra::~DatabaseCassandra()
{
    Close();
}

bool DatabaseCassandra::Open(const String& address, const String& username,
    const String& password)
{
    // Make sure any previous connection is closed.
    bool result = Close();

    // Now make a new connection.
    if(result)
    {
        mSession = cass_session_new();
        mCluster = cass_cluster_new();

        cass_cluster_set_contact_points(mCluster, address.C());

        if(!username.IsEmpty())
        {
            cass_cluster_set_credentials(mCluster, username.C(), password.C());
        }

        result = WaitForFuture(cass_session_connect(mSession, mCluster));
    }

    return result;
}

bool DatabaseCassandra::Close()
{
    bool result = true;

    if(nullptr != mSession)
    {
        result = WaitForFuture(cass_session_close(mSession));

        cass_session_free(mSession);
        mSession = nullptr;
    }

    if(nullptr != mCluster)
    {
        cass_cluster_free(mCluster);
        mCluster = nullptr;
    }

    if(result)
    {
        mError.Clear();
    }

    return result;
}

bool DatabaseCassandra::IsOpen() const
{
    return nullptr != mSession;
}

DatabaseQuery DatabaseCassandra::Prepare(const String& query)
{
    return DatabaseQuery(new DatabaseQueryCassandra(this), query);
}

bool DatabaseCassandra::Exists()
{
    DatabaseQuery q = Prepare(libcomp::String(
        "SELECT keyspace_name FROM system_schema.keyspaces WHERE keyspace_name = '%1';")
        .Arg(mKeyspace));
    if(!q.Execute())
    {
        LOG_CRITICAL("Failed to query for keyspace.\n");

        return false;
    }

    std::list<std::unordered_map<std::string, std::vector<char>>> results;
    q.Next();

    return q.GetRows(results) && results.size() > 0;
}

bool DatabaseCassandra::Setup()
{
    if(!IsOpen())
    {
        LOG_ERROR("Trying to setup a database that is not open!\n");

        return false;
    }

    if(!Exists())
    {
        // Delete the old keyspace if it exists.
        if(!Execute(libcomp::String("DROP KEYSPACE IF EXISTS %1;").Arg(mKeyspace)))
        {
            LOG_ERROR("Failed to delete old keyspace.\n");

            return false;
        }

        // Now re-create the keyspace.
        if(!Execute(libcomp::String("CREATE KEYSPACE %1 WITH REPLICATION = {"
            " 'class' : 'NetworkTopologyStrategy', 'datacenter1' : 1 };").Arg(mKeyspace)))
        {
            LOG_ERROR("Failed to create keyspace.\n");

            return false;
        }

        // Use the keyspace.
        if(!Use())
        {
            LOG_ERROR("Failed to use the keyspace.\n");

            return false;
        }

        if(UsingDefaultKeyspace() &&
            !Execute("CREATE TABLE objects ( uid uuid PRIMARY KEY, "
            "member_vars map<ascii, blob> );"))
        {
            LOG_ERROR("Failed to create the objects table.\n");

            return false;
        }
    }
    else if(!Use())
    {
        LOG_ERROR("Failed to use the existing keyspace.\n");

        return false;
    }

    LOG_DEBUG(libcomp::String("Database connection established to '%1' keyspace.\n")
        .Arg(mKeyspace));

    if(!VerifyAndSetupSchema())
    {
        LOG_ERROR("Schema verification and setup failed.\n");

        return false;
    }

    return true;
}

bool DatabaseCassandra::Use()
{
    // Use the keyspace.
    if(!Execute(libcomp::String("USE %1;").Arg(mKeyspace)))
    {
        LOG_ERROR("Failed to use the keyspace.\n");

        return false;
    }

    return true;
}

DatabaseQuery DatabaseCassandra::PrepareLoadObjectsQuery(bool& success,
    std::type_index type, const std::string& fieldName, const std::string& value)
{
    DatabaseQuery result(new DatabaseQueryCassandra(this));
    success = false;

    std::string fieldNameLower = libcomp::String(fieldName).ToLower().ToUtf8();
    bool loadByUUID = fieldNameLower == "uid";

    std::shared_ptr<libobjgen::MetaVariable> var;
    auto metaObject = PersistentObject::GetRegisteredMetadata(type);
    if(!loadByUUID)
    {
        for(auto iter = metaObject->VariablesBegin();
            iter != metaObject->VariablesEnd(); iter++)
        {
            if((*iter)->GetName() == fieldName)
            {
                var = (*iter);
                break;
            }
        }

        if(nullptr == var)
        {
            return result;
        }
    }

    std::string f = fieldNameLower;
    std::string v = value;
    if(nullptr != var)
    {
        f = libcomp::String(var->GetName()).ToLower().ToUtf8();
        if(var->GetMetaType() == libobjgen::MetaVariable::MetaVariableType_t::TYPE_STRING)
        {
            v = libcomp::String("'%1'").Arg(value).ToUtf8();
        }
    }

    //Build the query, if not loading by UUID filtering needs to b enabled
    std::stringstream ss;
    ss << "SELECT * FROM " << libcomp::String(metaObject->GetName()) .ToLower().ToUtf8()
        << " " << libcomp::String("WHERE %1 = %2%3")
                        .Arg(f).Arg(v).Arg(loadByUUID ? "" : " ALLOW FILTERING").ToUtf8();

    success = true;
    result.Prepare(ss.str());

    return result;
}

std::list<std::shared_ptr<PersistentObject>> DatabaseCassandra::LoadObjects(
    std::type_index type, const std::string& fieldName, const std::string& value)
{
    std::list<std::shared_ptr<PersistentObject>> objects;
    
    bool success;
    DatabaseQuery q = PrepareLoadObjectsQuery(success, type, fieldName, value);
    if(success && q.Execute())
    {
        auto metaObject = PersistentObject::GetRegisteredMetadata(type);

        std::list<std::unordered_map<std::string, std::vector<char>>> rows;
        q.Next();
        q.GetRows(rows);

        int failures = 0;
        if(rows.size() > 0)
        {
            for(auto row : rows)
            {
                auto obj = LoadSingleObjectFromRow(type, row);
                if(nullptr != obj)
                {
                    objects.push_back(obj);
                }
                else
                {
                    failures++;
                }
            }
        }

        if(failures > 0)
        {
            LOG_ERROR(libcomp::String("%1 '%2' row%3 failed to load.\n")
                .Arg(failures).Arg(metaObject->GetName()).Arg(failures != 1 ? "s" : ""));
        }
    }

    return objects;
}

std::shared_ptr<PersistentObject> DatabaseCassandra::LoadSingleObject(std::type_index type,
    const std::string& fieldName, const std::string& value)
{
    auto objects = LoadObjects(type, fieldName, value);

    return objects.size() > 0 ? objects.front() : nullptr;
}

std::shared_ptr<PersistentObject> DatabaseCassandra::LoadSingleObjectFromRow(
    std::type_index type, const std::unordered_map<std::string, std::vector<char>>& row)
{
    std::stringstream objstream(std::stringstream::out |
        std::stringstream::binary);

    libobjgen::UUID uuid;
    for(auto rowColumn : row)
    {
        std::vector<char>& value = rowColumn.second;
        if(rowColumn.first == "uid")
        {
            uuid = libobjgen::UUID(value);
        }
        else
        {
            /// @todo: get this to work for non-strings
            size_t strLength = value.size();
            objstream.write(reinterpret_cast<char*>(&strLength),
                sizeof(uint32_t));

            objstream.write(&value[0], (std::streamsize)strLength);
        }
    }

    auto obj = PersistentObject::New(type);
    if(!uuid.IsNull() && obj->Load(std::stringstream(objstream.str())))
    {
        obj->Register(obj, uuid);
    }
    else
    {
        obj = nullptr;
    }

    return obj;
}

bool DatabaseCassandra::VerifyAndSetupSchema()
{
    std::vector<std::shared_ptr<libobjgen::MetaObject>> metaObjectTables;
    for(auto registrar : PersistentObject::GetRegistry())
    {
        std::string source = registrar.second->GetSourceLocation();
        if(source == mKeyspace || (source.length() == 0 && UsingDefaultKeyspace()))
        {
            metaObjectTables.push_back(registrar.second);
        }
    }

    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> fieldMap;
    if(metaObjectTables.size() == 0)
    {
        return true;
    }
    else
    {
        LOG_DEBUG("Verifying database table structure.\n");

        std::stringstream ss;
        ss << "SELECT table_name, column_name, type"
            << " FROM system_schema.columns"
            " WHERE keyspace_name = '"
            << mKeyspace << "';";

        DatabaseQuery q = Prepare(ss.str());
        std::list<std::unordered_map<std::string, std::vector<char>>> results;
        if(!q.Execute() || !q.Next() || !q.GetRows(results))
        {
            LOG_CRITICAL("Failed to query for column schema.\n");

            return false;
        }

        for(auto row : results)
        {
            std::string tableName(&row["table_name"][0], row["table_name"].size());
            std::string colName(&row["column_name"][0], row["column_name"].size());
            std::string dataType(&row["type"][0], row["type"].size());

            std::unordered_map<std::string, std::string>& m = fieldMap[tableName];
            m[colName] = dataType;
        }
    }

    for(auto metaObjectTable : metaObjectTables)
    {
        auto metaObject = *metaObjectTable.get();
        auto objName = libcomp::String(metaObject.GetName())
                            .ToLower().ToUtf8();

        std::vector<std::shared_ptr<libobjgen::MetaVariable>> vars;
        for(auto iter = metaObject.VariablesBegin();
            iter != metaObject.VariablesEnd(); iter++)
        {
            std::string type = GetVariableType(*iter);
            if(type.empty())
            {
                LOG_ERROR(libcomp::String(
                    "Unsupported field type encountered: %1\n")
                    .Arg((*iter)->GetCodeType()));
                return false;
            }
            vars.push_back(*iter);
        }

        bool creating = false;
        bool archiving = false;
        auto tableIter = fieldMap.find(objName);
        if(tableIter == fieldMap.end())
        {
            creating = true;
        }
        else
        {
            std::unordered_map<std::string,
                std::string> columns = tableIter->second;
            if(columns.size() - 1 != vars.size()
                || columns.find("uid") == columns.end())
            {
                archiving = true;
            }
            else
            {
                columns.erase("uid");
                for(auto var : vars)
                {
                    auto name = libcomp::String(
                        var->GetName()).ToLower().ToUtf8();
                    auto type = GetVariableType(var);

                    if(columns.find(name) == columns.end()
                        || columns[name] != type)
                    {
                        archiving = true;
                    }
                }
            }
        }

        if(archiving)
        {
            LOG_DEBUG(libcomp::String("Archiving table '%1'...\n")
                .Arg(metaObject.GetName()));
                
            bool success = false;

            /// @todo: do this properly
            std::stringstream ss;
            ss << "DROP TABLE " << objName;
            success = Execute(ss.str());

            if(success)
            {
                LOG_DEBUG("Archiving complete\n");
            }
            else
            {
                LOG_ERROR("Archiving failed\n");
                return false;
            }

            creating = true;
        }
            
        if(creating)
        {
            LOG_DEBUG(libcomp::String("Creating table '%1'...\n")
                .Arg(metaObject.GetName()));

            bool success = false;

            std::stringstream ss;
            ss << "CREATE TABLE " << objName
                << " (uid uuid," << std::endl;
            std::vector<std::string> keys;
            keys.push_back("uid");
            for(int i = 0; i < vars.size(); i++)
            {
                auto var = vars[i];
                std::string type = GetVariableType(var);

                ss << var->GetName() << " " << type << "," << std::endl;
                if(var->IsLookupKey())
                {
                    keys.push_back(var->GetName());
                }
            }

            ss << "PRIMARY KEY(";
            for(int i = 0; i < keys.size(); i++)
            {
                if(i > 0)
                {
                    ss << ",";
                }
                ss << keys[i];
            }
            ss << "));";

            success = Execute(ss.str());

            if(success)
            {
                LOG_DEBUG("Creation complete\n");
            }
            else
            {
                LOG_ERROR("Creation failed\n");
                return false;
            }
        }
        else
        {
            LOG_DEBUG(libcomp::String("'%1': Verified\n")
                .Arg(metaObject.GetName()));
        }
    }

    return true;
}

bool DatabaseCassandra::UsingDefaultKeyspace()
{
    return mKeyspace == "comp_hack";
}

bool DatabaseCassandra::WaitForFuture(CassFuture *pFuture)
{
    bool result = true;

    cass_future_wait(pFuture);

    CassError errorCode = cass_future_error_code(pFuture);

    // Handle an error.
    if(CASS_OK != errorCode)
    {
        const char *szMessage;
        size_t messageLength;

        // Get.
        cass_future_error_message(pFuture, &szMessage, &messageLength);
  
        // Save.
        mError = String(szMessage, messageLength);

        result = false;
    }

    cass_future_free(pFuture);

    return result;
}

std::string DatabaseCassandra::GetVariableType(const std::shared_ptr
    <libobjgen::MetaVariable> var)
{
    switch(var->GetMetaType())
    {
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_STRING:
            return "text";
            break;
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_S8:
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_S16:
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_S32:
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_U8:
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_U16:
            return "int";
            break;
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_S64:
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_U32:
            return "bigint";
            break;
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_U64:
            /// @todo: Should this be a bigint too with conversion?
            return "bigint";
            break;
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_REF:
            return "uuid";
            break;
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_ARRAY:
        case libobjgen::MetaVariable::MetaVariableType_t::TYPE_LIST:
            return "list";
            break;
    }

    return "";
}

CassSession* DatabaseCassandra::GetSession() const
{
    return mSession;
}
