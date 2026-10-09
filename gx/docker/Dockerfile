FROM debian:12
# arm64 packages give the tests real programs to run (curl and its libraries),
# and gx itself the libraries it loads at run time (Mesa for EGL and GLES).
RUN dpkg --add-architecture arm64
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential gcc-aarch64-linux-gnu libc6-dev-arm64-cross qemu-user-static \
    flex bison bc libelf-dev python3 git make rsync cpio ca-certificates file gdb-multiarch curl:arm64 \
    libegl1:arm64 libgles2:arm64 libegl-mesa0:arm64 libgl1-mesa-dri:arm64 libegl-dev:arm64 libgles-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /project
