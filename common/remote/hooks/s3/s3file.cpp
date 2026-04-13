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

#include <aws/core/utils/stream/PreallocatedStreamBuf.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>

#include "platform.h"
#include "jlib.hpp"
#include "jfile.hpp"
#include "jlog.hpp"
#include "jptree.hpp"
#include "jplane.hpp"
#include "jregexp.hpp"

#include "s3file.hpp"
#include "s3utils.hpp"

#ifdef _MSC_VER
#undef GetObject
#endif

//---------------------------------------------------------------------------------------------------------------------
// Forward declarations
class S3File;

//---------------------------------------------------------------------------------------------------------------------
// S3FileReadIO

class S3FileReadIO : implements CInterfaceOf<IFileIO>
{
    Linked<S3File> file;
    FileIOStats stats;
    offset_t cachedFileSize;

public:
    S3FileReadIO(S3File * _file);
    virtual size32_t read(offset_t pos, size32_t len, void * data) override;
    virtual offset_t size() override;
    virtual void close() override {}
    virtual void flush() override {}
    virtual size32_t write(offset_t pos, size32_t len, const void * data) override { throwUnexpected(); }
    virtual void setSize(offset_t size) override { throwUnexpected(); }
    virtual unsigned __int64 getStatistic(StatisticKind kind) override { return stats.getStatistic(kind); }
    virtual IFile * queryFile() const override;

private:
    size32_t readFromS3(offset_t pos, size32_t len, void * data);
};

//---------------------------------------------------------------------------------------------------------------------
// S3MultipartUpload

class S3MultipartUpload
{
    StringAttr planeName;
    unsigned device;
    StringAttr bucket;
    StringAttr key;
    StringAttr uploadId;
    StringBuffer fullPath;
    Aws::Vector<Aws::S3::Model::CompletedPart> completedParts;
    unsigned partNumber = 1;
    bool active = false;

public:
    S3MultipartUpload(const char * _planeName, unsigned _device, const char * _bucket, const char * _key)
        : planeName(_planeName), device(_device), bucket(_bucket), key(_key)
    {
        fullPath.appendf("s3:%s/%s", _planeName, _key);
    }
    ~S3MultipartUpload()
    {
        if (active)
        {
            try { abort(); }
            catch (...) { ERRLOG("Failed to abort S3 multipart upload for %s", fullPath.str()); }
        }
    }

    void initiate();
    void uploadPart(const void * data, size32_t len);
    void complete();
    void abort();

private:
    Aws::S3::S3Client & getClient() { return getS3Client(planeName.str(), device); }
};

//---------------------------------------------------------------------------------------------------------------------
// S3FileWriteIO

class S3FileWriteIO : implements CInterfaceOf<IFileIO>
{
    static constexpr size32_t minMultipartSize = 5 * 1024 * 1024;   // 5MB — S3 minimum part size
    static constexpr size32_t writeBufferSize = 8 * 1024 * 1024;    // 8MB — flush threshold

    Linked<S3File> file;
    FileIOStats stats;
    std::unique_ptr<S3MultipartUpload> multipartUpload;
    MemoryBuffer pending;
    CriticalSection ioCS;
    bool closed = false;
    offset_t currentPos = 0;

public:
    S3FileWriteIO(S3File * _file);
    virtual void beforeDispose() override;
    virtual size32_t write(offset_t pos, size32_t len, const void * data) override;
    virtual void close() override;
    virtual void flush() override;
    virtual void setSize(offset_t size) override {}
    virtual size32_t read(offset_t pos, size32_t len, void * data) override { throwUnexpected(); }
    virtual offset_t size() override { throwUnexpected(); }
    virtual unsigned __int64 getStatistic(StatisticKind kind) override { return stats.getStatistic(kind); }
    virtual IFile * queryFile() const override;

private:
    void flushPending();
    void putObject(const void * data, size32_t len);
};

//---------------------------------------------------------------------------------------------------------------------
// S3File

class S3File : implements CInterfaceOf<IFile>
{
    friend class S3FileReadIO;
    friend class S3FileWriteIO;
    friend class S3DirectoryIterator;

    StringBuffer fullName;
    StringBuffer planeName;
    StringBuffer bucketName;
    StringBuffer keyName;
    unsigned device = 1;

    mutable CriticalSection metaCS;
    mutable bool haveMeta = false;
    mutable bool fileExists = false;
    mutable bool isDir = false;
    mutable offset_t fileSize = unknownFileSize;
    mutable time_t modifiedTime = 0;

public:
    S3File(const char * s3FileName);

    virtual const char * queryFilename() override { return fullName.str(); }
    virtual bool exists() override;
    virtual fileBool isDirectory() override;
    virtual fileBool isFile() override;
    virtual fileBool isReadOnly() override { return fileBool::foundYes; }
    virtual offset_t size() override;
    virtual bool getTime(CDateTime * createTime, CDateTime * modifiedTime, CDateTime * accessedTime) override;
    virtual bool getInfo(bool & isdir, offset_t & size, CDateTime & modtime) override;
    virtual IFileIO * open(IFOmode mode, IFEflags extraFlags = IFEnone) override;
    virtual IFileAsyncIO * openAsync(IFOmode mode) override { UNIMPLEMENTED; }
    virtual IFileIO * openShared(IFOmode mode, IFSHmode shmode, IFEflags extraFlags = IFEnone) override;
    virtual bool remove() override;
    virtual bool createDirectory() override { return true; }
    virtual IDirectoryIterator * directoryFiles(const char * mask, bool sub, bool includeDirs) override;

    // Not applicable to S3
    virtual bool setTime(const CDateTime *, const CDateTime *, const CDateTime *) override { UNIMPLEMENTED; }
    virtual void rename(const char *) override { UNIMPLEMENTED; }
    virtual void move(const char *) override { UNIMPLEMENTED; }
    virtual void setReadOnly(bool) override { UNIMPLEMENTED; }
    virtual void setFilePermissions(unsigned) override { UNIMPLEMENTED; }
    virtual bool setCompression(bool) override { UNIMPLEMENTED; }
    virtual offset_t compressedSize() override { UNIMPLEMENTED; }
    virtual unsigned getCRC() override { UNIMPLEMENTED; }
    virtual void setCreateFlags(unsigned short) override { UNIMPLEMENTED; }
    virtual void setShareMode(IFSHmode) override { UNIMPLEMENTED; }
    virtual IDirectoryDifferenceIterator * monitorDirectory(IDirectoryIterator *, const char *, bool, bool, unsigned, unsigned, Semaphore *) override { UNIMPLEMENTED; }
    virtual void copySection(const RemoteFilename &, offset_t, offset_t, offset_t, ICopyFileProgress *, CFflags) override { UNIMPLEMENTED; }
    virtual void copyTo(IFile *, size32_t, ICopyFileProgress *, bool, CFflags) override { UNIMPLEMENTED; }
    virtual IMemoryMappedFile * openMemoryMapped(offset_t, memsize_t, bool) override { UNIMPLEMENTED; }

protected:
    void ensureMetadata() const;
    void gatherMetadata() const;
    void invalidateMeta() { CriticalBlock block(metaCS); haveMeta = false; }
    Aws::S3::S3Client & getClient() const { return getS3Client(planeName.str(), device); }
};

//---------------------------------------------------------------------------------------------------------------------
// S3FileReadIO implementation

S3FileReadIO::S3FileReadIO(S3File * _file)
    : file(_file), cachedFileSize(_file->size()) {}

size32_t S3FileReadIO::read(offset_t pos, size32_t len, void * data)
{
    if (pos >= cachedFileSize)
        return 0;
    if (pos + len > cachedFileSize)
        len = (size32_t)(cachedFileSize - pos);
    if (len == 0)
        return 0;
    size32_t bytesRead = readFromS3(pos, len, data);
    stats.ioReads++;
    stats.ioReadBytes += bytesRead;
    return bytesRead;
}

size32_t S3FileReadIO::readFromS3(offset_t pos, size32_t len, void * data)
{
    const char * filename = file->queryFilename();
    CCycleTimer timer;
    size32_t bytesRead = retryS3("S3File::read", filename, [&]() -> size32_t
    {
        Aws::S3::Model::GetObjectRequest request;
        request.SetBucket(file->bucketName.str());
        request.SetKey(file->keyName.str());
        if (pos > 0 || len != cachedFileSize)
        {
            VStringBuffer range("bytes=%llu-%llu", (unsigned long long)pos, (unsigned long long)(pos + len - 1));
            request.SetRange(range.str());
        }
        auto outcome = file->getClient().GetObject(request);
        if (!outcome.IsSuccess())
        {
            auto & error = outcome.GetError();
            VStringBuffer msg("S3File::read failed for %s: %s - %s", filename,
                error.GetExceptionName().c_str(), error.GetMessage().c_str());
            throw std::runtime_error(msg.str());
        }
        auto & body = outcome.GetResult().GetBody();
        body.read((char *)data, len);
        return (size32_t)body.gcount();
    });
    stats.ioReadCycles += timer.elapsedCycles();
    return bytesRead;
}

offset_t S3FileReadIO::size() { return file->size(); }
IFile * S3FileReadIO::queryFile() const { return file.get(); }

//---------------------------------------------------------------------------------------------------------------------
// S3MultipartUpload implementation

void S3MultipartUpload::initiate()
{
    completedParts.reserve(200);
    retryS3Op("S3::CreateMultipartUpload", fullPath.str(), [&]()
    {
        Aws::S3::Model::CreateMultipartUploadRequest request;
        request.SetBucket(bucket.str());
        request.SetKey(key.str());
        auto outcome = getClient().CreateMultipartUpload(request);
        if (outcome.IsSuccess())
            uploadId.set(outcome.GetResult().GetUploadId().c_str());
        return outcome;
    });
    active = true;
}

void S3MultipartUpload::uploadPart(const void * data, size32_t len)
{
    assertex(active);
    retryS3Op("S3::UploadPart", fullPath.str(), [&]()
    {
        Aws::S3::Model::UploadPartRequest request;
        request.SetBucket(bucket.str());
        request.SetKey(key.str());
        request.SetUploadId(uploadId.str());
        request.SetPartNumber(partNumber);
        auto buf = Aws::New<Aws::Utils::Stream::PreallocatedStreamBuf>("s3", (unsigned char *)data, len);
        request.SetBody(Aws::MakeShared<Aws::IOStream>("s3", buf));
        request.SetContentLength(len);
        auto outcome = getClient().UploadPart(request);
        if (outcome.IsSuccess())
        {
            Aws::S3::Model::CompletedPart cp;
            cp.SetPartNumber(partNumber);
            cp.SetETag(outcome.GetResult().GetETag());
            completedParts.push_back(cp);
            partNumber++;
        }
        return outcome;
    });
}

void S3MultipartUpload::complete()
{
    if (!active) return;
    retryS3Op("S3::CompleteMultipartUpload", fullPath.str(), [&]()
    {
        Aws::S3::Model::CompletedMultipartUpload completedUpload;
        completedUpload.SetParts(completedParts);
        Aws::S3::Model::CompleteMultipartUploadRequest request;
        request.SetBucket(bucket.str());
        request.SetKey(key.str());
        request.SetUploadId(uploadId.str());
        request.SetMultipartUpload(completedUpload);
        return getClient().CompleteMultipartUpload(request);
    });
    active = false;
}

void S3MultipartUpload::abort()
{
    if (!active) return;
    retryS3Op("S3::AbortMultipartUpload", fullPath.str(), [&]()
    {
        Aws::S3::Model::AbortMultipartUploadRequest request;
        request.SetBucket(bucket.str());
        request.SetKey(key.str());
        request.SetUploadId(uploadId.str());
        return getClient().AbortMultipartUpload(request);
    });
    active = false;
}

//---------------------------------------------------------------------------------------------------------------------
// S3FileWriteIO implementation

S3FileWriteIO::S3FileWriteIO(S3File * _file) : file(_file) {}

void S3FileWriteIO::beforeDispose()
{
    try { close(); }
    catch (IException * e) { StringBuffer msg; e->errorMessage(msg); ERRLOG("S3 file disposal: %s", msg.str()); e->Release(); }
    catch (...) { ERRLOG("S3 file disposal failed for %s", file->queryFilename()); }
}

size32_t S3FileWriteIO::write(offset_t pos, size32_t len, const void * data)
{
    if (closed)
        throw makeStringException(-1, "Attempt to write to closed S3 file");
    if (len == 0)
        return 0;
    CriticalBlock block(ioCS);
    if (pos != currentPos)
        throw makeStringException(-1, "S3 file writer only supports sequential writes");
    file->invalidateMeta();
    CCycleTimer timer;
    pending.append(len, data);
    currentPos += len;
    while (pending.length() >= writeBufferSize)
        flushPending();
    stats.ioWrites++;
    stats.ioWriteBytes += len;
    stats.ioWriteCycles += timer.elapsedCycles();
    return len;
}

void S3FileWriteIO::flush()
{
    CriticalBlock block(ioCS);
    if (pending.length() >= minMultipartSize)
        flushPending();
}

void S3FileWriteIO::close()
{
    CriticalBlock block(ioCS);
    if (closed) return;
    if (multipartUpload)
    {
        if (pending.length())
        {
            multipartUpload->uploadPart(pending.toByteArray(), pending.length());
            pending.clear();
        }
        multipartUpload->complete();
        multipartUpload.reset();
    }
    else
    {
        putObject(pending.toByteArray(), pending.length());
        pending.clear();
    }
    closed = true;
}

void S3FileWriteIO::flushPending()
{
    if (pending.length() == 0) return;
    if (!multipartUpload)
    {
        multipartUpload = std::make_unique<S3MultipartUpload>(file->planeName.str(), file->device, file->bucketName.str(), file->keyName.str());
        multipartUpload->initiate();
    }
    size32_t flushLen = std::min(pending.length(), writeBufferSize);
    multipartUpload->uploadPart(pending.toByteArray(), flushLen);
    if (flushLen == pending.length())
        pending.clear();
    else
    {
        size32_t remaining = pending.length() - flushLen;
        memmove((void *)pending.toByteArray(), pending.toByteArray() + flushLen, remaining);
        pending.setLength(remaining);
    }
}

void S3FileWriteIO::putObject(const void * data, size32_t len)
{
    retryS3Op("S3::PutObject", file->queryFilename(), [&]()
    {
        Aws::S3::Model::PutObjectRequest request;
        request.SetBucket(file->bucketName.str());
        request.SetKey(file->keyName.str());
        auto buf = Aws::New<Aws::Utils::Stream::PreallocatedStreamBuf>("s3",
            reinterpret_cast<unsigned char *>(const_cast<void *>(data)), len);
        request.SetBody(Aws::MakeShared<Aws::IOStream>("s3", buf));
        request.SetContentLength(len);
        return file->getClient().PutObject(request);
    });
}

IFile * S3FileWriteIO::queryFile() const { return file.get(); }

//---------------------------------------------------------------------------------------------------------------------
// S3File implementation

S3File::S3File(const char * s3FileName) : fullName(s3FileName)
{
    if (!startsWith(fullName, s3FilePrefix))
        throw makeStringExceptionV(99, "Unexpected prefix on S3 filename %s", fullName.str());

    const char * filename = fullName.str() + strlen(s3FilePrefix);
    const char * slash = strchr(filename, '/');
    if (!slash)
        throw makeStringException(99, "Missing / in s3: file reference");

    planeName.append(slash - filename, filename);
    Owned<const IPropertyTree> plane = getStoragePlaneConfig(planeName, true);
    filename = slash + 1;

    unsigned numDevices = plane->getPropInt("@numDevices", 1);
    if (numDevices != 1)
    {
        if (filename[0] != 'd')
            throw makeStringExceptionV(99, "Expected a device number in the filename %s", fullName.str());
        char * endDevice = nullptr;
        device = strtol(filename + 1, &endDevice, 10);
        if ((device == 0) || (device > numDevices))
            throw makeStringExceptionV(99, "Device %d out of range for plane %s", device, planeName.str());
        if (!endDevice || (*endDevice != '/'))
            throw makeStringExceptionV(99, "Unexpected end of device partition %s", fullName.str());
        filename = endDevice + 1;
    }

    getClient(); // validate plane and device

    VStringBuffer childPath("storageapi/buckets[%u]", device);
    const char * bucket = plane->queryPropTree(childPath)->queryProp("@name");
    if (isEmptyString(bucket))
        throw makeStringExceptionV(99, "Missing bucket name for plane %s", planeName.str());
    bucketName.set(bucket);
    keyName.set(filename);
}

bool S3File::exists() { ensureMetadata(); return fileExists; }

fileBool S3File::isDirectory()
{
    ensureMetadata();
    return !fileExists ? fileBool::notFound : (isDir ? fileBool::foundYes : fileBool::foundNo);
}

fileBool S3File::isFile()
{
    ensureMetadata();
    return !fileExists ? fileBool::notFound : (!isDir ? fileBool::foundYes : fileBool::foundNo);
}

offset_t S3File::size() { ensureMetadata(); return fileSize; }

bool S3File::getTime(CDateTime * createTime, CDateTime * modifiedTime, CDateTime * accessedTime)
{
    ensureMetadata();
    if (createTime) createTime->set(this->modifiedTime);
    if (modifiedTime) modifiedTime->set(this->modifiedTime);
    if (accessedTime) accessedTime->clear();
    return fileExists;
}

bool S3File::getInfo(bool & isdir, offset_t & size, CDateTime & modtime)
{
    ensureMetadata();
    isdir = this->isDir;
    size = fileSize;
    modtime.set(this->modifiedTime);
    return fileExists;
}

IFileIO * S3File::open(IFOmode mode, IFEflags extraFlags)
{
    switch (mode)
    {
        case IFOread:   return exists() ? new S3FileReadIO(this) : nullptr;
        case IFOcreate:
        case IFOwrite:  return new S3FileWriteIO(this);
        default:        throw makeStringException(-1, "Unsupported file open mode for S3 file");
    }
}

IFileIO * S3File::openShared(IFOmode mode, IFSHmode, IFEflags extraFlags)
{
    return open(mode, extraFlags);
}

bool S3File::remove()
{
    try
    {
        retryS3Op("S3::DeleteObject", fullName.str(), [&]()
        {
            Aws::S3::Model::DeleteObjectRequest request;
            request.SetBucket(bucketName.str());
            request.SetKey(keyName.str());
            return getClient().DeleteObject(request);
        });
        CriticalBlock block(metaCS);
        haveMeta = true;
        fileExists = false;
        fileSize = 0;
        return true;
    }
    catch (...)
    {
        ERRLOG("S3 DeleteObject failed for %s", fullName.str());
        return false;
    }
}

void S3File::ensureMetadata() const
{
    CriticalBlock block(metaCS);
    if (haveMeta) return;
    gatherMetadata();
}

void S3File::gatherMetadata() const
{
    retryS3("S3File::gatherMetadata", fullName.str(), [&]()
    {
        Aws::S3::Model::HeadObjectRequest request;
        request.SetBucket(bucketName.str());
        request.SetKey(keyName.str());
        auto outcome = getClient().HeadObject(request);
        if (outcome.IsSuccess())
        {
            fileExists = true;
            fileSize = outcome.GetResult().GetContentLength();
            modifiedTime = outcome.GetResult().GetLastModified().Seconds();
            isDir = false;
        }
        else
        {
            auto & error = outcome.GetError();
            if (error.GetErrorType() == Aws::S3::S3Errors::NO_SUCH_KEY ||
                error.GetErrorType() == Aws::S3::S3Errors::RESOURCE_NOT_FOUND ||
                error.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND)
            {
                fileExists = false;
                fileSize = 0;
                modifiedTime = 0;
                isDir = false;
            }
            else
            {
                VStringBuffer msg("HeadObject failed for %s: %s - %s", fullName.str(),
                    error.GetExceptionName().c_str(), error.GetMessage().c_str());
                throw std::runtime_error(msg.str());
            }
        }
    });
    haveMeta = true;
}

//---------------------------------------------------------------------------------------------------------------------
// S3DirectoryIterator

class S3DirectoryIterator : implements IDirectoryIterator, public CInterface
{
public:
    IMPLEMENT_IINTERFACE;

    S3DirectoryIterator(S3File & _owner, const char * _mask, bool _sub, bool _includeDirs)
        : owner(&_owner), mask(_mask), sub(_sub), includeDirs(_includeDirs) {}

    virtual bool first() override { index = 0; entries.kill(); fetchEntries(); return isValid(); }
    virtual bool next() override { index++; return isValid(); }
    virtual bool isValid() override { return index < entries.ordinality(); }

    virtual IFile & query() override
    {
        Entry & e = entries.item(index);
        if (!e.file)
        {
            StringBuffer path;
            path.append(s3FilePrefix).append(owner->planeName).append("/").append(e.key);
            e.file.setown(createS3File(path.str()));
        }
        return *e.file;
    }

    virtual StringBuffer & getName(StringBuffer & buf) override
    {
        const char * key = entries.item(index).key.str();
        size_t keyLen = strlen(key);
        if (keyLen > 0 && key[keyLen - 1] == '/')
            keyLen--;
        const char * slash = nullptr;
        for (size_t i = keyLen; i > 0; i--)
            if (key[i - 1] == '/') { slash = key + i - 1; break; }
        return slash ? buf.append(keyLen - (slash + 1 - key), slash + 1) : buf.append(keyLen, key);
    }

    virtual bool isDir() override { return entries.item(index).isDir; }
    virtual __int64 getFileSize() override { return entries.item(index).size; }
    virtual bool getModifiedTime(CDateTime & ret) override
    {
        ret.clear();
        time_t t = entries.item(index).modified;
        if (t) ret.set(t);
        return t != 0;
    }

private:
    struct Entry : public CInterface
    {
        StringAttr key;
        Owned<IFile> file;
        offset_t size = 0;
        time_t modified = 0;
        bool isDir = false;
    };

    void fetchEntries()
    {
        StringBuffer prefix(owner->keyName);
        if (prefix.length() && prefix.charAt(prefix.length() - 1) != '/')
            prefix.append('/');

        VStringBuffer listCtx("s3://%s/%s", owner->bucketName.str(), prefix.str());
        Aws::String continuationToken;
        bool hasMore = true;
        while (hasMore)
        {
            Aws::S3::Model::ListObjectsV2Request request;
            request.SetBucket(owner->bucketName.str());
            request.SetPrefix(prefix.str());
            if (!sub) request.SetDelimiter("/");
            if (!continuationToken.empty()) request.SetContinuationToken(continuationToken);

            auto outcome = retryS3("S3::ListObjectsV2", listCtx.str(), [&]()
            {
                auto o = owner->getClient().ListObjectsV2(request);
                if (!o.IsSuccess())
                {
                    auto & error = o.GetError();
                    VStringBuffer msg("ListObjectsV2 failed for %s: %s - %s", listCtx.str(),
                        error.GetExceptionName().c_str(), error.GetMessage().c_str());
                    throw std::runtime_error(msg.str());
                }
                return o;
            });

            auto & result = outcome.GetResult();
            for (auto & obj : result.GetContents())
            {
                const Aws::String & key = obj.GetKey();
                if (key.length() == (size_t)prefix.length()) continue;
                const char * name = key.c_str() + prefix.length();
                if (mask.length() && !WildMatch(name, mask, false)) continue;
                Entry * e = new Entry;
                e->key.set(key.c_str());
                e->size = obj.GetSize();
                e->modified = obj.GetLastModified().Seconds();
                entries.append(*e);
            }
            if (includeDirs && !sub)
            {
                for (auto & cp : result.GetCommonPrefixes())
                {
                    Entry * e = new Entry;
                    e->key.set(cp.GetPrefix().c_str());
                    e->isDir = true;
                    entries.append(*e);
                }
            }
            hasMore = result.GetIsTruncated();
            continuationToken = result.GetNextContinuationToken();
        }
    }

    Linked<S3File> owner;
    StringAttr mask;
    bool sub;
    bool includeDirs;
    unsigned index = 0;
    CIArrayOf<Entry> entries;
};

IDirectoryIterator * S3File::directoryFiles(const char * mask, bool sub, bool includeDirs)
{
    return new S3DirectoryIterator(*this, mask ? mask : "", sub, includeDirs);
}

//---------------------------------------------------------------------------------------------------------------------
// Exported functions

extern S3FILE_API IFile * createS3File(const char * s3FileName)
{
    return new S3File(s3FileName);
}

extern S3FILE_API bool isS3FileName(const char * fileName)
{
    return !isEmptyString(fileName) && startsWith(fileName, s3FilePrefix) && strchr(fileName + s3FilePrefixLen, '/');
}
