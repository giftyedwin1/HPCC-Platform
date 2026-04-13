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

#ifndef S3FILE_HPP
#define S3FILE_HPP

#include "jfile.hpp"

#ifdef S3FILE_EXPORTS
#define S3FILE_API DECL_EXPORT
#else
#define S3FILE_API DECL_IMPORT
#endif

/*
 * S3 file access via storage planes
 *
 * Filenames: s3:<planeName>/<key>  or  s3:<planeName>/d<N>/<key> (multi-device)
 * Configuration from storage plane definitions (storageapi type "s3").
 */

extern "C" {
    extern S3FILE_API void installFileHook();
    extern S3FILE_API void removeFileHook();
    extern S3FILE_API IFile * createS3File(const char * s3FileName);
    extern S3FILE_API bool isS3FileName(const char * fileName);
};

#endif
