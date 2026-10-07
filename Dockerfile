# Two images, which ./sm64zune builds on first use (`docker build --target build|deploy`):
#
#   build    Wine and the tools that turn a ROM plus this repository into a Zune package
#   deploy   zune-deploy, which installs that package on a Zune HD over USB
#
# Neither image holds Microsoft's compiler, NVIDIA's shader compiler or anything from a ROM:
# `./sm64zune build` downloads the first two into build/toolchain and reads the ROM you give
# it. The deploy image holds what the zune-deploy repository ships, which includes the Zune's
# XNA runtime and USB handshake keys. Build these images yourself; do not publish them.

FROM mcr.microsoft.com/dotnet/sdk:10.0-noble AS dotnet
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake git libssl-dev \
    && rm -rf /var/lib/apt/lists/*
# zune-deploy by gigalasr, pinned; the commit also pins its MTP library submodule.
WORKDIR /zune-deploy
RUN git clone https://github.com/gigalasr/zune-deploy . \
    && git checkout --quiet eb5910901c143f5eb303c57086d1d3dbd58fcff1 \
    && git submodule update --init --recursive
RUN dotnet publish src/ZuneDeploy.CLI/ZuneDeploy.CLI.csproj --configuration Release \
        --runtime linux-x64 --self-contained false --output /out

FROM mcr.microsoft.com/dotnet/runtime:10.0-noble AS deploy
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 LD_LIBRARY_PATH=/opt/zune-deploy
WORKDIR /opt/zune-deploy
COPY --from=dotnet /out/ ./
COPY --from=dotnet /zune-deploy/docs/.mtpz-data /root/.mtpz-data
RUN ln -s libmtp-ng.so.4.6 libmtp-ng.so.4 \
    && test -s Assets/DeployableRuntimes/ZuneHD/mscorlib.dll
ENTRYPOINT ["dotnet", "/opt/zune-deploy/ZuneDeploy.CLI.dll"]

FROM debian:bookworm-slim AS build
# wine: runs the VC9 ARM compiler and NVIDIA's shader compiler (both 32-bit Windows programs).
# Second line: what sm64ex's own Dockerfile installs, for its build that extracts the ROM.
# Third line: for unpacking the downloaded installers and applying our patches.
RUN dpkg --add-architecture i386 \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        wine wine32 \
        bsdextrautils build-essential git libglew-dev libsdl2-dev python3 \
        ca-certificates cabextract msitools patch \
    && rm -rf /var/lib/apt/lists/*
ENV WINEARCH=win32 WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" PYTHONDONTWRITEBYTECODE=1
WORKDIR /repo
