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
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/CopyObjectRequest.h>
#include <aws/s3/model/UploadPartCopyRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>

#include "platform.h"
#include "jlib.hpp"
#include "jfile.hpp"
#include "jlog.hpp"
#include "jutil.hpp"

#include "s3file.hpp"
#include "s3utils.hpp"

//---------------------------------------------------------------------------------------------------------------------
// S3 API Copy Client — server-side copy via CopyObject / multipart copy

static constexpr offset_t maxSingleCopySize = (offset_t)5 * 1024 * 1024 * 1024;  // 5GB
static constexpr offset_t copyPartSize = (offset_t)500 * 1024 * 1024;             // 500MB

class S3APICopyClientOp : public CInterfaceOf<IAPICopyClientOp>
{
public:
    S3APICopyClientOp(const char * _srcBucket, const char * _srcKey,
                      const char * _tgtBucket, const char * _tgtKey,
                      const char * _srcPlane, unsigned _srcDevice,
                      const char * _tgtPlane, unsigned _tgtDevice)
        : srcBucket(_srcBucket), srcKey(_srcKey), tgtBucket(_tgtBucket), tgtKey(_tgtKey),
          srcPlane(_srcPlane), srcDevice(_srcDevice), tgtPlane(_tgtPlane), tgtDevice(_tgtDevice) {}

    virtual void startCopy(const char * source) override
    {
        try
        {
            retryS3("S3::HeadObject", srcKey.str(), [&]()
            {
                Aws::S3::Model::HeadObjectRequest headReq;
                headReq.SetBucket(srcBucket.str());
                headReq.SetKey(srcKey.str());
                auto outcome = getSrcClient().HeadObject(headReq);
                if (!outcome.IsSuccess())
                {
                    auto & error = outcome.GetError();
                    VStringBuffer msg("S3 copy: cannot stat %s/%s: %s - %s", srcBucket.str(), srcKey.str(),
                        error.GetExceptionName().c_str(), error.GetMessage().c_str());
                    throw std::runtime_error(msg.str());
                }
                srcSize = outcome.GetResult().GetContentLength();
            });
            if (srcSize <= maxSingleCopySize)
                doSimpleCopy();
            else
                doMultipartCopy();
            status = ApiCopyStatus::Success;
        }
        catch (...)
        {
            status = ApiCopyStatus::Failed;
            throw;
        }
    }

    virtual ApiCopyStatus getProgress(CDateTime & dateTime, int64_t & outputLength) override
    {
        dateTime.clear();
        outputLength = (status == ApiCopyStatus::Success) ? srcSize : 0;
        return status;
    }
    virtual ApiCopyStatus abortCopy() override { status = ApiCopyStatus::Aborted; return status; }
    virtual ApiCopyStatus getStatus() const override { return status; }

private:
    void doSimpleCopy()
    {
        StringBuffer rawSource, copySource;
        rawSource.appendf("%s/%s", srcBucket.str(), srcKey.str());
        encodeURL(copySource, rawSource.str());
        retryS3Op("S3::CopyObject", copySource.str(), [&]()
        {
            Aws::S3::Model::CopyObjectRequest request;
            request.SetBucket(tgtBucket.str());
            request.SetKey(tgtKey.str());
            request.SetCopySource(copySource.str());
            return getTgtClient().CopyObject(request);
        });
    }

    void doMultipartCopy()
    {
        Aws::String uploadId;
        retryS3("S3::CreateMultipartUpload", tgtKey.str(), [&]()
        {
            Aws::S3::Model::CreateMultipartUploadRequest initReq;
            initReq.SetBucket(tgtBucket.str());
            initReq.SetKey(tgtKey.str());
            auto outcome = getTgtClient().CreateMultipartUpload(initReq);
            if (!outcome.IsSuccess())
            {
                auto & error = outcome.GetError();
                VStringBuffer msg("Multipart copy initiate failed for %s/%s: %s - %s",
                    tgtBucket.str(), tgtKey.str(), error.GetExceptionName().c_str(), error.GetMessage().c_str());
                throw std::runtime_error(msg.str());
            }
            uploadId = outcome.GetResult().GetUploadId();
        });

        Aws::Vector<Aws::S3::Model::CompletedPart> parts;
        StringBuffer rawSource, copySource;
        rawSource.appendf("%s/%s", srcBucket.str(), srcKey.str());
        encodeURL(copySource, rawSource.str());

        try
        {
            unsigned partNum = 1;
            for (offset_t pos = 0; pos < srcSize; pos += copyPartSize, partNum++)
            {
                offset_t end = std::min(pos + copyPartSize - 1, srcSize - 1);
                VStringBuffer range("bytes=%llu-%llu", (unsigned long long)pos, (unsigned long long)end);
                retryS3("S3::UploadPartCopy", copySource.str(), [&]()
                {
                    Aws::S3::Model::UploadPartCopyRequest partReq;
                    partReq.SetBucket(tgtBucket.str());
                    partReq.SetKey(tgtKey.str());
                    partReq.SetUploadId(uploadId);
                    partReq.SetPartNumber(partNum);
                    partReq.SetCopySource(copySource.str());
                    partReq.SetCopySourceRange(range.str());
                    auto outcome = getTgtClient().UploadPartCopy(partReq);
                    if (!outcome.IsSuccess())
                    {
                        auto & error = outcome.GetError();
                        VStringBuffer msg("UploadPartCopy failed for %s: %s - %s", copySource.str(),
                            error.GetExceptionName().c_str(), error.GetMessage().c_str());
                        throw std::runtime_error(msg.str());
                    }
                    Aws::S3::Model::CompletedPart cp;
                    cp.SetPartNumber(partNum);
                    cp.SetETag(outcome.GetResult().GetCopyPartResult().GetETag());
                    parts.push_back(cp);
                });
            }
            retryS3Op("S3::CompleteMultipartUpload", tgtKey.str(), [&]()
            {
                Aws::S3::Model::CompletedMultipartUpload completed;
                completed.SetParts(parts);
                Aws::S3::Model::CompleteMultipartUploadRequest completeReq;
                completeReq.SetBucket(tgtBucket.str());
                completeReq.SetKey(tgtKey.str());
                completeReq.SetUploadId(uploadId);
                completeReq.SetMultipartUpload(completed);
                return getTgtClient().CompleteMultipartUpload(completeReq);
            });
        }
        catch (...)
        {
            Aws::S3::Model::AbortMultipartUploadRequest abortReq;
            abortReq.SetBucket(tgtBucket.str());
            abortReq.SetKey(tgtKey.str());
            abortReq.SetUploadId(uploadId);
            getTgtClient().AbortMultipartUpload(abortReq);
            throw;
        }
    }

    Aws::S3::S3Client & getSrcClient() { return getS3Client(srcPlane.str(), srcDevice); }
    Aws::S3::S3Client & getTgtClient() { return getS3Client(tgtPlane.str(), tgtDevice); }

    StringAttr srcBucket, srcKey, tgtBucket, tgtKey, srcPlane, tgtPlane;
    unsigned srcDevice, tgtDevice;
    offset_t srcSize = 0;
    ApiCopyStatus status = ApiCopyStatus::NotStarted;
};

class S3APICopyClient : public CInterfaceOf<IAPICopyClient>
{
    Linked<IStorageApiInfo> source, target;
public:
    S3APICopyClient(IStorageApiInfo * _source, IStorageApiInfo * _target)
        : source(_source), target(_target) {}
    virtual const char * name() const override { return "S3 API copy client"; }
    virtual IAPICopyClientOp * startCopy(const char * srcPath, unsigned srcStripeNum,
                                         const char * tgtPath, unsigned tgtStripeNum) const override
    {
        Owned<S3APICopyClientOp> op = new S3APICopyClientOp(
            source->queryStorageContainerName(srcStripeNum), srcPath,
            target->queryStorageContainerName(tgtStripeNum), tgtPath,
            source->queryPlaneName(), srcStripeNum,
            target->queryPlaneName(), tgtStripeNum);
        op->startCopy(srcPath);
        return op.getClear();
    }
};

//---------------------------------------------------------------------------------------------------------------------
// File hook — routes s3: filenames to S3File, provides copy client

static bool isS3Type(const char * type) { return type && strieq(type, "s3"); }

class S3FileHook : public CInterfaceOf<IContainedFileHook>
{
public:
    virtual IFile * createIFile(const char * fileName) override
    {
        return isS3FileName(fileName) ? createS3File(fileName) : nullptr;
    }
    virtual IAPICopyClient * getCopyApiClient(IStorageApiInfo * source, IStorageApiInfo * target) override
    {
        if (source && target && isS3Type(source->getStorageType()) && isS3Type(target->getStorageType()))
            return new S3APICopyClient(source, target);
        return nullptr;
    }
};

static S3FileHook * s3FileHook = nullptr;
static CriticalSection hookCS;

//---------------------------------------------------------------------------------------------------------------------
// Exported functions

extern S3FILE_API void installFileHook()
{
    CriticalBlock block(hookCS);
    if (!s3FileHook)
    {
        s3FileHook = new S3FileHook;
        addContainedFileHook(s3FileHook);
    }
}

extern S3FILE_API void removeFileHook()
{
    CriticalBlock block(hookCS);
    if (s3FileHook)
    {
        removeContainedFileHook(s3FileHook);
        delete s3FileHook;
        s3FileHook = nullptr;
    }
}

//---------------------------------------------------------------------------------------------------------------------
// AWS SDK lifecycle

static Aws::SDKOptions awsOptions;

MODULE_INIT(INIT_PRIORITY_STANDARD)
{
    awsOptions.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Warn;
#ifndef _WIN32
    awsOptions.httpOptions.installSigPipeHandler = true;
#endif
    Aws::InitAPI(awsOptions);
    LOG(MCdebugProgress, "AWS SDK initialized for S3 file operations");
    return true;
}

MODULE_EXIT()
{
    cleanupS3Clients();
    removeFileHook();
    Aws::ShutdownAPI(awsOptions);
    LOG(MCdebugProgress, "AWS SDK shutdown for S3 file operations");
}
