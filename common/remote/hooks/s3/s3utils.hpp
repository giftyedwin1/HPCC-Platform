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

#ifndef S3UTILS_HPP
#define S3UTILS_HPP

#include "jlib.hpp"
#include "jlog.hpp"
#include "jstring.hpp"
#include "jexcept.hpp"
#include "jutil.hpp"

#include <aws/s3/S3Client.h>

/*
 * Common utility functions and constants shared by S3 file and API copy implementations
 */

constexpr const char * s3FilePrefix = "s3:";
constexpr size_t s3FilePrefixLen = 3;
constexpr unsigned defaultMaxRetries = 3;

// S3 client cache — one client per (plane, device) pair
Aws::S3::S3Client & getS3Client(const char * planeName, unsigned device);
void cleanupS3Clients();

// Retry with exponential backoff — rethrows IException*, retries std::exception
template <typename Fn>
static auto retryS3(const char * op, const char * context, Fn && fn) -> decltype(fn())
{
    unsigned attempt = 0;
    for (;;)
    {
        try
        {
            return fn();
        }
        catch (IException *)
        {
            throw;
        }
        catch (const std::exception & e)
        {
            attempt++;
            VStringBuffer msg("%s failed (attempt %u/%u) for %s: %s", op, attempt, defaultMaxRetries, context, e.what());
            OWARNLOG("%s", msg.str());
            if (attempt >= defaultMaxRetries)
                throw makeStringException(-1, msg);
            Sleep((1U << attempt) * 100 + (getRandom() % 100));
        }
    }
}

// Retry helper that converts AWS SDK error outcomes to exceptions for the retry loop
template <typename Fn>
static void retryS3Op(const char * op, const char * context, Fn && fn)
{
    retryS3(op, context, [&]()
    {
        auto outcome = fn();
        if (!outcome.IsSuccess())
        {
            auto & error = outcome.GetError();
            VStringBuffer msg("%s failed for %s: %s - %s", op, context,
                error.GetExceptionName().c_str(), error.GetMessage().c_str());
            throw std::runtime_error(msg.str());
        }
        LOG(MCdebugProgress, "%s succeeded for %s", op, context);
    });
}

#endif
