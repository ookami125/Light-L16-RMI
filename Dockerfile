FROM ubuntu:22.04

ARG NDK_VERSION=r16b
ARG NDK_ZIP=android-ndk-${NDK_VERSION}-linux-x86_64.zip

ENV DEBIAN_FRONTEND=noninteractive
ENV ANDROID_NDK_HOME=/opt/android-ndk-${NDK_VERSION}
ENV PATH=${ANDROID_NDK_HOME}/toolchains/aarch64-linux-android-4.9/prebuilt/linux-x86_64/bin:${PATH}

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        binutils-aarch64-linux-gnu \
        gcc-aarch64-linux-gnu \
        libc6-dev-arm64-cross \
        make \
        unzip \
        wget \
        xxd \
    && rm -rf /var/lib/apt/lists/*

RUN wget -q https://dl.google.com/android/repository/${NDK_ZIP} \
    && unzip -q ${NDK_ZIP} -d /opt \
    && rm -f ${NDK_ZIP}

WORKDIR /src

CMD ["bash"]
