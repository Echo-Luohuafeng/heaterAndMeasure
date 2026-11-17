# 官方 latest 标签跟踪 ESP-IDF master；默认固定为已发布的正式版本。
ARG DOCKER_TAG=v6.1.1
FROM espressif/idf:${DOCKER_TAG}

# 数字 GID 必须与宿主机设备组一致；本机 dialout 的 GID 为 18。
ARG DEVICE_GID=18

ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

# 与 Espressif 的 Dev Container 模板一致，补充 Linux 设备管理依赖。
RUN apt-get update \
    && apt-get install -y --no-install-recommends udev \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04 可能已包含 UID/GID 1000 的 ubuntu 用户，复用它避免冲突。
# Linux 宿主机的最终 UID/GID 由 Dev Containers 的 updateRemoteUserUID 映射。
RUN if id -u ubuntu >/dev/null 2>&1; then \
        usermod --login vscode --home /home/vscode --move-home --shell /bin/bash ubuntu \
        && groupmod --new-name vscode ubuntu; \
    else \
        groupadd --gid 1000 vscode \
        && useradd --uid 1000 --gid vscode --create-home --shell /bin/bash vscode; \
    fi \
    && if getent group "${DEVICE_GID}" >/dev/null; then \
        device_group="$(getent group "${DEVICE_GID}" | cut -d: -f1)"; \
    else \
        groupadd --gid "${DEVICE_GID}" host-device; \
        device_group=host-device; \
    fi \
    && usermod --append --groups "${device_group}" vscode

USER vscode

# 家目录由 UID/GID 映射一并调整，缓存和终端配置均由开发用户写入。
ENV CCACHE_DIR=/home/vscode/.cache/ccache
RUN mkdir -p /home/vscode/.cache/ccache \
    && printf '\n. /opt/esp/idf/export.sh >/dev/null\n' >> /home/vscode/.bashrc

ENTRYPOINT ["/opt/esp/entrypoint.sh"]
CMD ["/bin/bash"]
