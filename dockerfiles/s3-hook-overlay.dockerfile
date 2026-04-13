FROM --platform=linux/amd64 hpccsystems/platform-build-base-ubuntu-22.04:916fbc9c AS builder

# Copy HPCC source needed for headers
COPY system/jlib /hpcc-dev/src/system/jlib
COPY system/include /hpcc-dev/src/system/include
COPY system/security/cryptohelper /hpcc-dev/src/system/security/cryptohelper
COPY common/remote/hooks/s3/s3file.cpp /hpcc-dev/src/s3file.cpp
COPY common/remote/hooks/s3/s3file.hpp /hpcc-dev/src/s3file.hpp
COPY common/remote/hooks/s3/s3api.cpp /hpcc-dev/src/s3api.cpp
COPY common/remote/hooks/s3/s3utils.cpp /hpcc-dev/src/s3utils.cpp
COPY common/remote/hooks/s3/s3utils.hpp /hpcc-dev/src/s3utils.hpp
COPY common/remote/hooks/s3/jplane_compat.hpp /hpcc-dev/src/jplane_compat.hpp

RUN mkdir -p /hpcc-dev/build && echo '#define BUILD_TAG "s3-dev"' > /hpcc-dev/build/build-config.h

# Patch for stock 10.2.22 API compatibility
RUN cd /hpcc-dev/src && \
    sed -i 's|#include "jplane.hpp"|#include "jplane_compat.hpp"|' s3file.cpp s3utils.cpp && \
    sed -i 's|if (source && target && isS3Type(source->getStorageType()) && isS3Type(target->getStorageType()))|if (false)|' s3api.cpp && \
    sed -i 's|source->queryPlaneName()|""|g; s|target->queryPlaneName()|""|g' s3api.cpp

WORKDIR /hpcc-dev

RUN g++ -shared -fPIC -o /hpcc-dev/libs3file.so \
    /hpcc-dev/src/s3utils.cpp \
    /hpcc-dev/src/s3file.cpp \
    /hpcc-dev/src/s3api.cpp \
    -DS3FILE_EXPORTS -D_CONTAINERIZED -DINLINE_GET_CYCLES_NOW \
    -I/hpcc-dev/src/system/include \
    -I/hpcc-dev/src/system/jlib \
    -I/hpcc-dev/src/system/security/cryptohelper \
    -I/hpcc-dev/src \
    -I/hpcc-dev/build \
    -I/hpcc-dev/vcpkg_installed/x64-linux-dynamic/include \
    -L/hpcc-dev/vcpkg_installed/x64-linux-dynamic/lib \
    -Wl,-rpath,/opt/HPCCSystems/lib \
    -Wl,-soname,libs3file.so \
    -laws-cpp-sdk-s3 -laws-cpp-sdk-core -ldl \
    -std=c++17 -O2 -DNDEBUG

FROM --platform=linux/amd64 hpccsystems/platform-core:10.2.22
USER root
COPY --from=builder /hpcc-dev/libs3file.so /opt/HPCCSystems/filehooks/libs3file.so
USER hpcc
