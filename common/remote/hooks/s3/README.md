# S3 Direct API File Hook

Storage hook for accessing S3 objects as HPCC files using the AWS C++ SDK.

## File Format

```
s3:<planeName>/<key>
s3:<planeName>/d<N>/<key>    (multi-device)
```

## Architecture

The hook registers via `installFileHook()` at process startup. When any HPCC
component calls `createIFile("s3:...")`, the hook intercepts it and returns an
`S3File` that implements `IFile`. Opening for read returns `S3FileReadIO`,
opening for write returns `S3FileWriteIO` — both implement `IFileIO`.

## Features

- Read via `GetObject` with byte-range requests
- Write coalescing — buffers small writes, auto-selects between:
  - `PutObject` for files under 8MB (single HTTP request)
  - Multipart upload for files 8MB+ (8MB parts)
- 0-byte objects created for empty file parts (Thor multi-part compatibility)
- Server-side copy via `CopyObject` / multipart copy for large files
- Directory listing via `ListObjectsV2` with prefix/delimiter filtering
- Connection pooling — one S3 client per plane+device combination
- Retry with exponential backoff and jitter for all S3 operations
- IAM credential chain (IRSA on EKS, env vars, ~/.aws/credentials)

## Configuration

Storage plane in Helm values:

```yaml
storage:
  planes:
  - name: s3data
    prefix: "s3:s3data"
    category: data
    storageapi:
      type: s3
      region: us-east-1
      buckets:
      - name: my-bucket
        secret: my-secret          # optional — omit for IAM roles
```

## Files

| File | Description |
|------|-------------|
| `s3file.cpp` | S3File, S3FileReadIO, S3FileWriteIO, S3MultipartUpload, S3DirectoryIterator |
| `s3file.hpp` | Public API — `installFileHook`, `createS3File`, `isS3FileName` |
| `s3api.cpp` | S3FileHook registration, S3APICopyClient, AWS SDK lifecycle |
| `s3utils.cpp` | S3ClientManager — client cache and creation |
| `s3utils.hpp` | Retry helpers, constants, `getS3Client()` API |
| `s3fileTests.cpp` | Unit tests — URL validation |
| `jplane_compat.hpp` | Build shim for cross-version `getStoragePlaneConfig` resolution |
| `CMakeLists.txt` | Build config — links against jlib + aws-cpp-sdk-{s3,core} |

## Build

Built as part of the platform via CMake, or as a standalone overlay — see
`dockerfiles/s3-hook-overlay.dockerfile` and `helm/examples/s3/README.md`.
