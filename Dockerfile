FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        git \
        gdb-multiarch \
        qemu-system-misc \
        gcc-riscv64-linux-gnu \
        binutils-riscv64-linux-gnu \
        bear \
        python3 \
        python3-pexpect \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# 冒烟：版本号对不上就在构建阶段暴露
RUN riscv64-linux-gnu-gcc --version | head -1 \
 && qemu-system-riscv64 --version | head -1 \
 && gdb-multiarch --version | head -1

CMD ["/bin/bash"]
