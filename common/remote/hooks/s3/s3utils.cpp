/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2026 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/s3/S3Client.h>

#include "platform.h"
#include "jlib.hpp"
#include "jmutex.hpp"
#include "jptree.hpp"
#include "jplane.hpp"
#include "jsecrets.hpp"

#include "s3utils.hpp"

//---------------------------------------------------------------------------------------------------------------------
// S3 Client cache key

struct S3ClientKey
{
    std::string planeName;
    unsigned device;
    bool operator==(const S3ClientKey & other) const
    {
        return (planeName == other.planeName) && (device == other.device);
    }
};

namespace std {
    template<> struct hash<S3ClientKey>
    {
        size_t operator()(const S3ClientKey & key) const noexcept
        {
            unsigned h = hashc((const unsigned char *)key.planeName.c_str(), key.planeName.length(), fnvInitialHash32);
            return hashvalue(key.device, h);
        }
    };
}

//---------------------------------------------------------------------------------------------------------------------
// S3 Client Manager

class S3ClientManager
{
    mutable CriticalSection cs;
    std::unordered_map<S3ClientKey, std::unique_ptr<Aws::S3::S3Client>> clients;

public:
    Aws::S3::S3Client & getClient(const char * planeName, unsigned device)
    {
        CriticalBlock block(cs);
        S3ClientKey key{planeName, device};
        auto it = clients.find(key);
        if (it != clients.end())
            return *(it->second);
        auto client = createClient(planeName, device);
        auto & ref = *client;
        clients.emplace(key, std::move(client));
        return ref;
    }

    void cleanup()
    {
        CriticalBlock block(cs);
        clients.clear();
    }

private:
    std::unique_ptr<Aws::S3::S3Client> createClient(const char * planeName, unsigned device)
    {
        Owned<const IPropertyTree> plane = getStoragePlaneConfig(planeName, true);
        const IPropertyTree * storageapi = plane->queryPropTree("storageapi");
        if (!storageapi)
            throw makeStringExceptionV(99, "No storage api defined for plane %s", planeName);
        const char * type = storageapi->queryProp("@type");
        if (!type || !strieq(type, "s3"))
            throw makeStringExceptionV(99, "Storage api type for plane %s is not s3", planeName);

        VStringBuffer childPath("buckets[%u]", device);
        const IPropertyTree * bucketInfo = storageapi->queryPropTree(childPath);
        if (!bucketInfo)
            throw makeStringExceptionV(99, "Missing bucket specification for device %u in plane %s", device, planeName);

        Aws::Client::ClientConfiguration clientConfig;
        const char * regionStr = storageapi->queryProp("@region");
        if (!isEmptyString(regionStr))
            clientConfig.region = regionStr;
        const char * endpointStr = storageapi->queryProp("@endpoint");
        if (!isEmptyString(endpointStr))
            clientConfig.endpointOverride = endpointStr;

        clientConfig.scheme = storageapi->getPropBool("@useSSL", true) ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
        clientConfig.connectTimeoutMs = storageapi->getPropInt("@connectTimeoutMs", 30000);
        clientConfig.requestTimeoutMs = storageapi->getPropInt("@requestTimeoutMs", 300000);
        clientConfig.maxConnections = storageapi->getPropInt("@maxConnections", 100);
        clientConfig.retryStrategy = std::make_shared<Aws::Client::DefaultRetryStrategy>(storageapi->getPropInt("@maxRetries", defaultMaxRetries));

        const char * secretName = bucketInfo->queryProp("@secret");
        if (!isEmptyString(secretName))
        {
            StringBuffer accessKey, keyId;
            getSecretValue(accessKey, "storage", secretName, "aws-access-key", true);
            getSecretValue(keyId, "storage", secretName, "aws-key-id", true);
            return std::make_unique<Aws::S3::S3Client>(
                Aws::Auth::AWSCredentials(keyId.str(), accessKey.str()),
                clientConfig,
                Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::RequestDependent,
                true, Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION::NOT_SET);
        }
        else
        {
            return std::make_unique<Aws::S3::S3Client>(
                std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>(),
                clientConfig,
                Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::RequestDependent,
                true, Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION::NOT_SET);
        }
    }
};

static S3ClientManager & queryS3ClientManager()
{
    static S3ClientManager manager;
    return manager;
}

Aws::S3::S3Client & getS3Client(const char * planeName, unsigned device)
{
    return queryS3ClientManager().getClient(planeName, device);
}

void cleanupS3Clients()
{
    queryS3ClientManager().cleanup();
}
