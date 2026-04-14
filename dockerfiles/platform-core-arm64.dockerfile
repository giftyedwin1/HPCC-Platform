# ARM64 platform-core image built from published .deb package
# Usage: docker build --platform linux/arm64 -t hpcc-platform-core:10.2.24-arm64 -f dockerfiles/platform-core-arm64.dockerfile .

ARG HPCC_VERSION=10.2.24-1
FROM arm64v8/ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -y && \
    apt-get install --no-install-recommends -y \
    curl \
    default-jdk \
    elfutils \
    expect \
    g++ \
    git \
    locales \
    jq \
    libjemalloc2 \
    openssh-client \
    openssh-server \
    python3 \
    python3-dev \
    psmisc \
    r-base-core \
    r-cran-rcpp \
    r-cran-inline \
    rsync \
    zip \
    clang \
    libcppunit-1.15-0 \
    dnsutils \
    gdb \
    nano && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# kubectl — arch-aware
RUN curl -LO "https://storage.googleapis.com/kubernetes-release/release/v1.29.7/bin/linux/arm64/kubectl" && \
    chmod +x ./kubectl && mv ./kubectl /usr/local/bin

# git-lfs — arch-aware
RUN curl -LO "https://packagecloud.io/github/git-lfs/packages/ubuntu/jammy/git-lfs_3.7.1_arm64.deb/download" && \
    dpkg -i download && rm download

# Locale
RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8

# HPCC user
RUN groupadd -g 10001 hpcc && \
    useradd -s /bin/bash -m -r -N -c "hpcc runtime User" -u 10000 -g hpcc hpcc && \
    passwd -l hpcc

RUN mkdir -p /var/lib/HPCCSystems /var/log/HPCCSystems /var/lock/HPCCSystems /var/run/HPCCSystems && \
    chown hpcc:hpcc /var/lib/HPCCSystems /var/log/HPCCSystems /var/lock/HPCCSystems /var/run/HPCCSystems

# Install HPCC platform from published ARM64 k8s .deb
ARG HPCC_VERSION
RUN curl -LO "https://github.com/hpcc-systems/HPCC-Platform/releases/download/community_${HPCC_VERSION}/hpccsystems-platform-community_${HPCC_VERSION}jammy_aarch64_k8s.deb" && \
    dpkg -i --force-depends hpccsystems-platform-community_${HPCC_VERSION}jammy_aarch64_k8s.deb || true && \
    apt-get update -y && apt-get install -f -y && \
    rm -f hpccsystems-platform-community_${HPCC_VERSION}jammy_aarch64_k8s.deb && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

USER hpcc
ENV PATH="/opt/HPCCSystems/bin:${PATH}"
ENV HPCC_containerized=1
WORKDIR /var/lib/HPCCSystems

ENTRYPOINT ["/bin/bash", "--login", "-c"]
